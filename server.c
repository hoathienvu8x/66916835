/*
 * https://stackoverflow.com/questions/66916835/c-confused-by-epoll-and-socket-fd-on-linux-systems-and-async-threads
 * gcc -Og -g -std=gnu99 -Wall -Wextra -Werror -pedantic 66916835.c -I. -ldl -lpthread -lm -o 66916835
 */
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/timerfd.h>
#include <netdb.h>

#ifndef NDEBUG
  #define PRERF "(errno=%d) %s\n"
  #define PREAR(NUM) NUM, strerror(NUM)
#endif
#define EPOLL_MAP_TO_NOP (0u)
#define EPOLL_MAP_SHIFT  (1u) /* Shift to cover reserved value MAP_TO_NOP */
#define array_size(a) (sizeof(a) / sizeof(*a))

#ifndef DEFAULT_TIMEOUT
  #define DEFAULT_TIMEOUT (3000)
#endif

#ifndef MAX_EVENTS
  #define MAX_EVENTS (32)
#endif

#ifndef BUFFER_SIZE
  #define BUFFER_SIZE (1024)
#endif

#ifndef BIND_TO_PORT
  #define BIND_TO_PORT (1234)
#endif

#ifndef LISTEN_BACKLOG
  #define LISTEN_BACKLOG (10)
#endif

#define MAX_CLIENT_MAP (10000)
#define MAX_CLIENT_SLOTS (10)
#define MAX_IPADDR_LEN sizeof("xxx.xxx.xxx.xxx")

struct client_slot {
  bool      is_used;
  int       client_fd;
  char      src_ip[MAX_IPADDR_LEN];
  uint16_t  src_port;
  uint16_t  my_index;
  int       client_state;
};

struct tcp_state {
  bool                stop;
  int                 tcp_fd;
  int                 epoll_fd;
  int                 time_fd;
  uint16_t            client_c;
  struct client_slot  clients[MAX_CLIENT_SLOTS];

  /*
   * Map the file descriptor to client_slot array index
   * Note: We assume there is no file descriptor greater than 10000.
   *
   * You must adjust this in production.
   */
  uint32_t            client_map[MAX_CLIENT_MAP];
};


static int my_epoll_add(int epoll_fd, int fd, uint32_t events) {
  struct epoll_event event;
  /* Shut the valgrind up! */
  memset(&event, 0, sizeof(struct epoll_event));

  event.events  = events;
  event.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
    #ifndef NDEBUG
    printf("epoll_ctl(EPOLL_CTL_ADD): " PRERF, PREAR(errno));
    #endif
    return -1;
  }
  return 0;
}

static int my_epoll_delete(int epoll_fd, int fd) {
  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
    #ifndef NDEBUG
    printf("epoll_ctl(EPOLL_CTL_DEL): " PRERF, PREAR(errno));
    #endif
    return -1;
  }
  return 0;
}

static int close_socket(int fd) {
  shutdown(fd, SHUT_RDWR);
  return close(fd);
}

static const char *convert_addr_ntop(
  struct sockaddr_in *addr, char *src_ip_buf
) {
  const char *ret;
  in_addr_t saddr = addr->sin_addr.s_addr;
  ret = inet_ntop(AF_INET, &saddr, src_ip_buf, MAX_IPADDR_LEN);
  if (ret == NULL) {
    #ifndef NDEBUG
    printf("inet_ntop(): " PRERF, PREAR((errno ? errno : EINVAL)));
    #endif
    return NULL;
  }
  return ret;
}

static int accept_new_client(int tcp_fd, struct tcp_state *state) {
  int client_fd, ret = -1;
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  uint16_t src_port;
  size_t i;
  const char *src_ip;
  char src_ip_buf[MAX_IPADDR_LEN];
  const size_t client_slot_num = array_size(state->clients);

  memset(&addr, 0, sizeof(addr));
  client_fd = accept(tcp_fd, (struct sockaddr *)&addr, &addr_len);
  if (client_fd < 0) {
    if (errno == EAGAIN)
      return 0;
    #ifndef NDEBUG
    /* Error */
    printf("accept(): " PRERF, PREAR(errno));
    #endif
    return -1;
  }

  src_port = ntohs(addr.sin_port);
  src_ip   = convert_addr_ntop(&addr, src_ip_buf);
  if (!src_ip) {
    #ifndef NDEBUG
    printf("Cannot parse source address\n");
    #endif
    goto out_close;
  }

  /*
   * Find unused client slot.
   *
   * In real world application, you don't want to iterate
   * the whole array, instead you can use stack data structure
   * to retrieve unused index in O(1).
   *
   */
  for (i = 0; i < client_slot_num; i++) {
    struct client_slot *client = &state->clients[i];

    if (!client->is_used) {
      /*
       * We found unused slot.
       */

      client->client_fd = client_fd;
      if (!memcpy(client->src_ip, src_ip_buf, sizeof(src_ip_buf))) {
        #ifndef NDEBUG
        printf("memcpy(): " PRERF, PREAR(errno));
        #endif
        ret = -1;
        goto out_close;
      }
      client->src_port = src_port;
      client->is_used = true;
      client->my_index = i;

      /*
       * We map the client_fd to client array index that we accept
       * here.
       */
      state->client_map[client_fd] = client->my_index + EPOLL_MAP_SHIFT;

      /*
       * Let's tell to `epoll` to monitor this client file descriptor.
       */
      if ((ret = my_epoll_add(
        state->epoll_fd, client_fd, EPOLLET | EPOLLIN
      )) < 0) {
        goto out_close;
      }
      #ifndef NDEBUG
      printf("Client %s:%u has been accepted!\n", src_ip, src_port);
      #endif
      return 0;
    }
  }
  #ifndef NDEBUG
  printf("Sorry, can't accept more client at the moment, slot is full\n");
  #endif

out_close:
  close_socket(client_fd);
  return ret;
}

static void handle_client_event(
  int client_fd, uint32_t revents, struct tcp_state *state
) {
  ssize_t recv_ret;
  char buffer[BUFFER_SIZE];
  const uint32_t err_mask = EPOLLERR | EPOLLHUP;
  /*
   * Read the mapped value to get client index.
   */
  uint32_t index = state->client_map[client_fd] - EPOLL_MAP_SHIFT;
  struct client_slot *client = &state->clients[index];

  if (revents & err_mask)
    goto close_conn;

  recv_ret = recv(client_fd, buffer, sizeof(buffer), 0);
  if (recv_ret == 0)
    goto close_conn;

  if (recv_ret < 0) {
    if (errno == EAGAIN)
      return;
    #ifndef NDEBUG
    /* Error */
    printf("recv(): " PRERF, PREAR(errno));
    #endif
    goto close_conn;
  }

  /*
   * Safe printing
   */
  buffer[recv_ret] = '\0';
  if (buffer[recv_ret - 1] == '\n') {
    buffer[recv_ret - 1] = '\0';
  }
  #ifndef NDEBUG
  printf(
    "Client %s:%u sends: \"%s\"\n",
    client->src_ip, client->src_port, buffer
  );
  #endif
  return;

close_conn:
  #ifndef NDEBUG
  printf(
    "Client %s:%u has closed its connection\n",
    client->src_ip, client->src_port
  );
  #endif
  my_epoll_delete(state->epoll_fd, client_fd);
  close_socket(client_fd);
  client->is_used = false;
  return;
}

static int event_loop(struct tcp_state *state) {
  int ret = 0, i;
  int epoll_ret;
  int epoll_fd = state->epoll_fd;
  struct epoll_event events[MAX_EVENTS];
  #ifndef NDEBUG
  printf("Entering event loop...\n");
  #endif
  while (!state->stop) {
    /*
     * I sleep on `epoll_wait` and the kernel will wake me up
     * when event comes to my monitored file descriptors, or
     * when the timeout reached.
     */
    epoll_ret = epoll_wait(epoll_fd, events, MAX_EVENTS, DEFAULT_TIMEOUT);

    if (epoll_ret == 0) {
        /*
         *`epoll_wait` reached its timeout
         */
        #ifndef NDEBUG
        printf("I don't see any event within %d milliseconds\n", DEFAULT_TIMEOUT);
        #endif
        continue;
    }

    if (epoll_ret == -1) {
      if (errno == EINTR) {
        #ifndef NDEBUG
        printf("Something interrupted me!\n");
        #endif
        continue;
      }
      /* Error */
      ret = -1;
      #ifndef NDEBUG
      printf("epoll_wait(): " PRERF, PREAR(errno));
      #endif
      break;
    }

    for (i = 0; i < epoll_ret; i++) {
      int fd = events[i].data.fd;

      if (fd == state->tcp_fd) {
        /*
         * A new client is connecting to us...
         */
        if (accept_new_client(fd, state) < 0) {
          ret = -1;
          goto out;
        }
        continue;
      }
      // [Epoll Timmer](https://github.com/stsaz/kernel-queue-the-complete-guide/blob/master/epoll-timer.c)
      if (fd == state->time_fd) {
        static int n;
        unsigned long long val;
        if (read(fd, &val, 8) > 0) {
          printf("Received timerfd event via epoll: %d\n", n++);
        }
        continue;
      }
      /*
       * We have event(s) from client, let's call `recv()` to read it.
       */
      handle_client_event(fd, events[i].events, state);
    }
  }

out:
  return ret;
}

static int init_epoll(struct tcp_state *state) {
  int epoll_fd;
  #ifndef NDEBUG
  printf("Initializing epoll_fd...\n");
  #endif
  /* The epoll_create argument is ignored on modern Linux */
  epoll_fd = epoll_create(255);
  if (epoll_fd < 0) {
    #ifndef NDEBUG
    printf("epoll_create(): " PRERF, PREAR(errno));
    #endif
    return -1;
  }

  state->epoll_fd = epoll_fd;
  return 0;
}

static int init_socket(struct tcp_state *state) {
  int ret, yes = 1, tcp_fd = -1;
  struct addrinfo hints, *results, *try;
  char port[20] = {0};
  const char *bind_addr = "0.0.0.0";

  #ifndef NDEBUG
  printf("Creating TCP socket...\n");
  #endif

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_flags = AI_PASSIVE;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (snprintf(port, sizeof(port) - 1, "%d", BIND_TO_PORT) <= 0) {
    #ifndef NDEBUG
    printf("snprintf(): " PRERF, PREAR(errno));
    #endif
    return -1;
  }

  if (getaddrinfo(bind_addr, port, &hints, &results) != 0) {
    #ifndef NDEBUG
    printf("getaddrinfo(): " PRERF, PREAR(errno));
    #endif
    return -1;
  }

  for (try = results; try != NULL; try = try->ai_next) {
    // [create the socket in non-blocking mode](https://stackoverflow.com/a/63348937)
    tcp_fd = socket(
      try->ai_family, try->ai_socktype | SOCK_NONBLOCK, try->ai_protocol
    );
    if (tcp_fd < 0)
      continue;

    if ((ret = setsockopt(
      tcp_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes)
    )) < 0) {
      #ifndef NDEBUG
      printf("setsockopt(SO_REUSEADDR): " PRERF, PREAR(errno));
      #endif
      freeaddrinfo(results);
      goto out;
    }

    if ((ret = bind(tcp_fd, try->ai_addr, try->ai_addrlen)) < 0) {
      #ifndef NDEBUG
      printf("bind(): " PRERF, PREAR(errno));
      #endif
      freeaddrinfo(results);
      goto out;
    }
    break;
  }

  freeaddrinfo(results);

  if (try == NULL) {
    ret = -1;
    goto out;
  }

  ret = listen(tcp_fd, LISTEN_BACKLOG);
  if (ret < 0) {
    ret = -1;
    #ifndef NDEBUG
    printf("listen(): " PRERF, PREAR(errno));
    #endif
    goto out;
  }

  /*
   * Add `tcp_fd` to epoll monitoring.
   *
   * If epoll returned tcp_fd in `events` then a client is
   * trying to connect to us.
   */
  ret = my_epoll_add(state->epoll_fd, tcp_fd, EPOLLIN | EPOLLOUT | EPOLLET);
  if (ret < 0) {
    ret = -1;
    goto out;
  }
  #ifndef NDEBUG
  printf("Listening on %s:%u...\n", bind_addr, BIND_TO_PORT);
  #endif
  state->tcp_fd = tcp_fd;
  return 0;

out:
  close_socket(tcp_fd);
  return ret;
}

static void init_state(struct tcp_state *state) {
  const size_t client_slot_num = array_size(state->clients);
  const uint16_t client_map_num = array_size(state->client_map);
  size_t i;
  for (i = 0; i < client_slot_num; i++) {
    state->clients[i].is_used = false;
    state->clients[i].client_fd = -1;
  }

  for (i = 0; i < client_map_num; i++) {
    state->client_map[i] = EPOLL_MAP_TO_NOP;
  }
  state->time_fd = -1;
}

static int tcp_state_init_periodic(struct tcp_state *state) {
  int time_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (time_fd < 0) return -1;
  state->time_fd = time_fd;
  my_epoll_add(state->epoll_fd, time_fd, EPOLLET | EPOLLIN);

  struct itimerspec its;
  its.it_value.tv_sec = 1;
  its.it_value.tv_nsec = 0;
  its.it_interval = its.it_value;

  if (timerfd_settime(state->time_fd, 0, &its, NULL) != 0) {
    if (close_socket(state->time_fd) != 0) {
      #ifndef NDEBUG
      printf("close() " PRERF, PREAR(errno));
      #endif
    } else {
      state->time_fd = -1;
    }
    return -1;
  }

  return 0;
}

static void tcp_state_destroy(struct tcp_state *state) {
  if (state->tcp_fd != -1) {
    close_socket(state->tcp_fd);
  }
  if (state->time_fd != -1) {
    close_socket(state->time_fd);
  }
  const size_t client_slot_num = array_size(state->clients);
  size_t i;
  for (i = 0; i < client_slot_num; i++) {
    close_socket(state->clients[i].client_fd);
  }
}

int main(void) {
  int ret;
  struct tcp_state state;

  init_state(&state);

  ret = init_epoll(&state);
  if (ret != 0)
    goto out;

  ret = init_socket(&state);
  if (ret != 0)
    goto out;

  tcp_state_init_periodic(&state);

  state.stop = false;

  ret = event_loop(&state);

  tcp_state_destroy(&state);

out:
  /*
   * You should write a cleaner here.
   *
   * Close all client file descriptors and release
   * some resources you may have.
   *
   * You may also want to set interrupt handler
   * before the event_loop.
   *
   * For example, if you get SIGINT or SIGTERM
   * set `state->stop` to true, so that it exits
   * gracefully.
   */
  return ret;
}
