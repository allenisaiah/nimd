#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_BOARD_SIZE 10
#define MAX_FIELDS 3
#define MAX_GAMES 50
#define MAX_MESSAGE_LENGTH 104
#define MAX_NAME_LENGTH 72

typedef struct {
  pid_t pid;
  char name1[MAX_NAME_LENGTH + 1];
  char name2[MAX_NAME_LENGTH + 1];
} ActiveGame;

typedef struct {
  char type[5];
  int field_count;
  char *fields[MAX_FIELDS];
} Message;

typedef struct {
  int fd;
  char name[MAX_NAME_LENGTH + 1];
} Player;

typedef struct {
  Player *player1;
  Player *player2;
  int board[5];
  int turn;
} Game;

typedef struct {
  int fd;
  char buf[128];
  size_t buf_length;
  Player *player;
  int player_number;
} Connection;

static ActiveGame active_games[MAX_GAMES];
static Player *waiting_player = NULL;
static int active_count = 0;

/*
 * This function creates a TCP socket listening on the port specified
 * by the user in the main() function.
 */
int create_listen_socket(int port) {
  // IPv4, TCP, default
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("Error when creating a listening socket");
    return -1;
  }

  // reuse ports in TIME_WAIT state (mostly useful during testing)
  int opt = 1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("Error when setting socket options");
    close(listen_fd);
    return -1;
  }

  // specify address for socket
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);

  // assign address:port to the socket
  if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("Error while binding");
    close(listen_fd);
    return -1;
  }

  // max of 10 potential connections to socket
  if (listen(listen_fd, 10) < 0) {
    perror("Error while listening from socket");
    close(listen_fd);
    return -1;
  }

  return listen_fd;
}

/*
 * This short function accepts a TCP client and returns it.
 */
int accept_client(int listen_fd){
  struct sockaddr_in client_address;
  socklen_t address_length = sizeof(client_address);

  // accept the client connection
  int client_fd = accept(listen_fd, (struct sockaddr *)&client_address, &address_length);
  if (client_fd < 0) {
    perror("Error while accepting client");
    return -1;
  }

  // convert to readable string for logging purposes
  char ip_address[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_address.sin_addr, ip_address, sizeof(ip_address));
  printf("Incoming connection from %s:%d\n", ip_address, ntohs(client_address.sin_port));

  return client_fd;
}

/*
 * This function parses messages sent by clients in form
 * 0|ML|TYPE|FIELD1|FIELD2|...|FIELD_N|
 * where 0 is the version, ML is the user-declared length of the message,
 * TYPE is the type of the message, and the rest of the fields correspond
 * to whatever syntax is required by the message type.
 *
 * Requirements
 * 1. Message length needs to at least have 7 characters.
 * 2. Version must be 0.
 * 3. ML must be 2 digits.
 * 4. Message length does not exceed 104 bytes.
 * 5. Message ends with a vertical bar.
 * 6. TYPE must be 4 digits.
 * 7. Fields must end with a vertical bar.
 *
 * We can consider a message to be in the form of a header and payload,
 * H|HH|PPPP|P|P|...|P|
 * where the first two fields are header fields (always 5 bytes including
 * field terminators), and the rest are part of the payload fields.
 */
int parse_message(char *message, size_t message_length, Message *out, size_t *counted) {
  // requirement 1 validation
  if (message_length < 7) {
    return 0;
  }

  // requirement 2 validation
  if (message[0] != '0' || message[1] != '|') {
    return -1;
  }

  // requirement 3 validation
  if (!isdigit(message[2]) || !isdigit(message[3]) || message[4] != '|') {
    return -1;
  }

  // extract message length to int from ML field
  int declared_length = (message[2]-'0') * 10 + (message[3]-'0');
  size_t total_length = 5 + declared_length;
  if (message_length < total_length) {
    return 0;
  }

  // requirement 4 validation
  if (total_length > MAX_MESSAGE_LENGTH) {
    return -1;
  }

  // requirement 5 validation
  if (message[total_length-1] != '|') {
    return -1;
  }

  // requirement 6 validation
  char *payload = message + 5;
  size_t payload_length = declared_length;
  if (payload_length < 5) {
    return -1;
  }

  memcpy(out->type, payload, 4);
  out->type[4] = '\0';

  if (payload[4] != '|') {
    return -1;
  }

  char *field = payload + 5;
  size_t bytes_counted = 5;
  out->field_count = 0;

  // validate fields 1 to n
  while (bytes_counted < payload_length) {
    // requirement 7 validation
    char *field_terminator = memchr(field, '|', payload_length - bytes_counted);
    if (!field_terminator) {
      return -1;
    }

    // ensure we do not go on forever
    if (out->field_count >= MAX_FIELDS) {
      return -1;
    }

    // store field in message struct
    *field_terminator = '\0';
    out->fields[out->field_count] = field;
    out->field_count++;

    // preparation for next loop
    size_t field_length = (field_terminator - field) + 1;
    bytes_counted += field_length;
    field = field_terminator + 1;
  }

  if (bytes_counted != payload_length) {
    return -1;
  }

  *counted = total_length;
  return 1;
}

/*
 * This short function sends a WAIT message to the given file descriptor
 * and verifies that the number of bytes sent matches the length of the
 * message.
 */
int send_wait_message(int fd) {
  char *message = "0|05|WAIT|";
  int message_length = strlen(message);
  ssize_t bytes_sent = send(fd, message, message_length, 0);

  return (bytes_sent == message_length) ? 0 : -1;
}

/*
 * This function sends a FAIL message to the given file descriptor and
 * verifies that the number of bytes sent matches the length of the
 * message.
 */
int send_fail_message(int fd, char *error_message) {
  char message[MAX_MESSAGE_LENGTH];
  int message_length = snprintf(message, sizeof(message), "0|00|FAIL|%s|", error_message);

  if (message_length < 0 || message_length >= (int)sizeof(message)) {
    return -1;
  }

  // exclude header (first two fields as described in parse_message())
  int payload_length = message_length - 5;
  if (payload_length < 0 || payload_length > 99) {
    return -1;
  }

  // replace |00| with |XX|, where XX is the actual payload length
  message[2] = '0' + (payload_length / 10);
  message[3] = '0' + (payload_length % 10);

  ssize_t bytes_sent = send(fd, message, message_length, 0);
  return (bytes_sent == message_length) ? 0 : -1;
}

/*
 * This function sends a NAME message to the given file descriptor and
 * verifies that the number of bytes sent matches the length of the
 * message.
 */
int send_name_message(int fd, int player_number, char *opponent) {
  char message[MAX_MESSAGE_LENGTH];
  int message_length = snprintf(message, sizeof(message),
                                "0|00|NAME|%d|%s|", player_number, opponent);

  if (message_length < 0 || message_length >= (int)sizeof(message)) {
    return -1;
  }

  // exclude header (first two fields as described in parse_message())
  int payload_length = message_length - 5;
  if (payload_length < 0 || payload_length > 99) {
    return -1;
  }

  // replace |00| with |XX|, where XX is the actual payload length
  message[2] = '0' + (payload_length / 10);
  message[3] = '0' + (payload_length % 10);

  ssize_t bytes_sent = send(fd, message, message_length, 0);
  return (bytes_sent == message_length) ? 0 : -1;
}

/*
 * This function sends a PLAY message to the given file descriptor and
 * verifies that the number of bytes sent matches the length of the
 * message.
 */
int send_play_message(int fd, int player_number, int board[5]) {
  char board_str[MAX_BOARD_SIZE];
  snprintf(board_str, sizeof(board_str), "%d %d %d %d %d",
           board[0], board[1], board[2], board[3], board[4]);

  char message[MAX_MESSAGE_LENGTH];
  int message_length = snprintf(message, sizeof(message),
                                "0|00|PLAY|%d|%s|", player_number, board_str);

  if (message_length < 0 || message_length >= (int)sizeof(message)) {
    return -1;
  }

  // exclude header (first two fields as described in parse_message())
  int payload_length = message_length - 5;
  if (payload_length < 0 || payload_length > 99) {
    return -1;
  }

  // replace |00| with |XX|, where XX is the actual payload length
  message[2] = '0' + (payload_length / 10);
  message[3] = '0' + (payload_length % 10);

  ssize_t bytes_sent = send(fd, message, message_length, 0);
  return (bytes_sent == message_length) ? 0 : -1;
}

/*
 * This function sends an OVER message to the given file descriptor and
 * verifies that the number of bytes sent matches the length of the
 * message.
 */
int send_over_message(int fd, int winner, int board[5], int forfeit) {
  char board_str[MAX_BOARD_SIZE];
  snprintf(board_str, sizeof(board_str), "%d %d %d %d %d",
           board[0], board[1], board[2], board[3], board[4]);

  char message[MAX_MESSAGE_LENGTH];
  int message_length = snprintf(message, sizeof(message),
                                "0|00|OVER|%d|%s|%s|", winner, board_str, forfeit ? "Forfeit" : "");

  if (message_length < 0 || message_length >= (int)sizeof(message)) {
    return -1;
  }

  // exclude header (first two fields as described in parse_message())
  int payload_length = message_length - 5;
  if (payload_length < 0 || payload_length > 99) {
    return -1;
  }

  // replace |00| with |XX|, where XX is the actual payload length
  message[2] = '0' + (payload_length / 10);
  message[3] = '0' + (payload_length % 10);

  ssize_t bytes_sent = send(fd, message, message_length, 0);
  return (bytes_sent == message_length) ? 0 : -1;
}

/* This complex function runs a loop that receives messages between
 * two sockets until someone disconnects, a forfeit occurs, or the
 * game is won normally. It uses Connection structs to hold connections
 * from both players, and runs infinitely until someone disconnects,
 * a forfeit occurs, or the game is won in a normal manner.
 *
 * ERROR                MEANING                                   CLOSE?
 * 10 Invalid           Message cannot be read                    Close
 * 21 Long Name         Player name too long                      Close
 * 22 Already Playing   Player already playing a game             Close
 * 23 Already Open      OPEN sent more than once                  Close
 * 24 Not Playing       MOVE sent before NAME                     Close
 * 31 Impatient         MOVE sent during opponent's turn          No
 * 32 Pile Index        Specified pile out of range               No
 * 33 Quantity          Number to remove too large or too small   No
 */
int run_game_loop(Game *game) {
  Connection connection[2] = {0};

  // player 1 connection info
  connection[0].fd = game->player1->fd;
  connection[0].buf_length = 0;
  connection[0].player = game->player1;
  connection[0].player_number = 1;

  // player 2 connection info
  connection[1].fd = game->player2->fd;
  connection[1].buf_length = 0;
  connection[1].player = game->player2;
  connection[1].player_number = 2;

  pid_t pid = getpid();

  // this loop runs until someone disconnects, a forfeit occurs,
  // or the game is won normally
  for (;;) {
    // track set of file descriptors
    fd_set set;
    FD_ZERO(&set);
    FD_SET(connection[0].fd, &set);
    FD_SET(connection[1].fd, &set);

    int maxfd;
    if (connection[0].fd > connection[1].fd) {
      maxfd = connection[0].fd;
    } else {
      maxfd = connection[1].fd;
    }

    int result = select(maxfd + 1, &set, NULL, NULL, NULL);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("Error while calling select");
      return -1;
    }

    // check who has data
    for (int i = 0; i < 2; i++) {
      if (!FD_ISSET(connection[i].fd, &set)) {
        continue;
      }

      // read data
      ssize_t bytes_received = recv(connection[i].fd, connection[i].buf + connection[i].buf_length,
                                    sizeof(connection[i].buf) - connection[i].buf_length, 0);

      // disconnection is an automatic loss by forfeit
      if (bytes_received <= 0) {
        int loser  = connection[i].player_number;
        int winner = (loser == 1 ? 2 : 1);
        Player *winner_player = (winner == 1 ? game->player1 : game->player2);

        printf("[PID %d] FORFEIT: %s (P%d) disconnected. Winner is %s (P%d).\n",
               (int)pid, connection[i].player->name, loser, winner_player->name,
               winner);

        send_over_message(winner_player->fd, winner, game->board, 1);
        return -1;
      }

      connection[i].buf_length += (size_t)bytes_received;

      // parse a message from the player
      for (;;) {
        Message message;
        size_t used;
        int result = parse_message(connection[i].buf, connection[i].buf_length, &message, &used);

        if (result == 0) {
          break;
        } else if (result < 0) {
          // badly parsed message (i.e. parse_message returned -1)
          int loser  = connection[i].player_number;
          int winner = (loser == 1 ? 2 : 1);
          Player *winner_player = (winner == 1 ? game->player1 : game->player2);

          printf("[PID %d] FORFEIT: %s (P%d) sent message with bad syntax. Winner is %s (P%d).\n",
                 (int)pid, connection[i].player->name, loser, winner_player->name, winner);

          send_fail_message(connection[i].fd, "10 Invalid");
          send_over_message(winner_player->fd, winner, game->board, 1);
          return -1;
        }

        // pull message from buffer
        memmove(connection[i].buf, connection[i].buf + used, connection[i].buf_length - used);
        connection[i].buf_length -= used;

        int player_number = connection[i].player_number;
        Player *current = connection[i].player;
        Player *other = (player_number == 1 ? game->player2 : game->player1);

        // if the message is anything but a move, automatic forfeit
        if (strcmp(message.type, "MOVE") != 0) {
          int loser  = player_number;
          int winner = (loser == 1 ? 2 : 1);

          printf("[PID %d] FORFEIT: %s (P%d) sent unexpected message type '%s'. Winner is %s (P%d).\n",
                 (int)pid, current->name, loser, message.type, other->name, winner);

          if (strcmp(message.type, "OPEN") == 0) {
            send_fail_message(current->fd, "23 Already Open");
          } else {
            send_fail_message(current->fd, "10 Invalid");
          }

          send_over_message(other->fd, winner, game->board, 1);
          return -1;
        }

        // impatient error code, not an automatic forfeit
        if (player_number != game->turn) {
          printf("[PID %d] IMPATIENT: %s (P%d) tried to MOVE out of turn.\n",
                 (int)pid, current->name, player_number);

          send_fail_message(current->fd, "31 Impatient");
          continue;
        }

        if (message.field_count != 2) {
          int loser = player_number;
          int winner = (loser == 1 ? 2 : 1);

          printf("[PID %d] FORFEIT: %s (P%d) sent MOVE with %d fields (expected 2). Winner is %s (P%d).\n",
                 (int)pid, current->name, loser, message.field_count, other->name, winner);

          send_fail_message(current->fd, "10 Invalid");
          send_over_message(other->fd, winner, game->board, 1);
          return -1;
        }

        int pile = atoi(message.fields[0]);
        int quantity = atoi(message.fields[1]);

        // ensure within pile bounds, not an automatic forfeit
        if (pile < 0 || pile >= 5) {
          printf("[PID %d] Invalid move from %s (P%d). Pile %d does not exist.\n",
                 (int)pid, current->name, player_number, pile + 1);

          send_fail_message(current->fd, "32 Pile Index");
          continue;
        }

        // ensure within quantity bounds, not an automatic forfeit
        if (quantity <= 0 || quantity > game->board[pile]) {
          printf("[PID %d] Invalid move from %s (P%d). Pile %d has %d stones (tried removing %d).\n",
                 (int)pid, current->name, player_number, pile + 1, game->board[pile], quantity);

          send_fail_message(current->fd, "33 Quantity");
          continue;
        }

        // apply changes according to player's move
        game->board[pile] -= quantity;

        printf("[PID %d] TURN: %s (P%d) removed %d stones from pile %d. Board is [%d %d %d %d %d].\n",
               (int)pid, current->name, player_number, quantity, pile + 1,
               game->board[0], game->board[1], game->board[2], game->board[3], game->board[4]);

        bool empty = true;
        for (int j = 0; j < 5; j++) {
          if (game->board[j] != 0) {
            empty = false;
            break;
          }
        }

        // winning condition
        if (empty) {
          printf("[PID %d] OVER: Winner is %s. Final board is [%d %d %d %d %d].\n",
                 (int)pid, current->name,
                 game->board[0], game->board[1], game->board[2], game->board[3], game->board[4]);

          send_over_message(current->fd, player_number, game->board, 0);
          send_over_message(other->fd, player_number, game->board, 0);
          return 0;
        }

        // otherwise, we just flip the turn back to the other player
        game->turn = (game->turn == 1 ? 2 : 1);

        send_play_message(game->player1->fd, game->turn, game->board);
        send_play_message(game->player2->fd, game->turn, game->board);
      }
    }
  }
}

/*
 * This function cycles through the array of active games and ensures
 * that the name is not already taken.
 */
int name_in_active_game(char *name) {
  if (waiting_player) {
    char tmp;
    ssize_t bytes_received = recv(waiting_player->fd, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);

    // errno flags are necessary to prevent kicking off another player
    // with a duplicate name
    if (bytes_received == 0 ||
        (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      close(waiting_player->fd);
      free(waiting_player);
      waiting_player = NULL;
    }
  }

  if (waiting_player && strcmp(waiting_player->name, name) == 0) {
    return 1;
  }

  for (int i = 0; i < active_count; i++) {
    if (strcmp(active_games[i].name1, name) == 0 ||
        strcmp(active_games[i].name2, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/*
 * This function attempts to register a child game. It fails if the
 * maximum number of games allowed has been reached. Otherwise,
 * it copies the child PID and the names of the players into the
 * ActiveGame struct at the "active_count"th (i.e. the next unpopulated
 * index of our global array).
 */
void register_child_game(pid_t pid, char *name1, char *name2) {
  if (active_count >= MAX_GAMES) {
    fprintf(stderr, "Cannot populate more than %d games.\n", MAX_GAMES);
    return;
  }

  active_games[active_count].pid = pid;
  strncpy(active_games[active_count].name1, name1, MAX_NAME_LENGTH);
  strncpy(active_games[active_count].name2, name2, MAX_NAME_LENGTH);
  active_games[active_count].name1[MAX_NAME_LENGTH] = '\0';
  active_games[active_count].name2[MAX_NAME_LENGTH] = '\0';
  active_count++;
}

/*
 * This function removes a child game. It cycles through the array of
 * active games until a match of the PID is found, then performs the
 * appropriate logic to free the space for another game to be registered.
 */
void remove_child_game(pid_t pid) {
  for (int i = 0; i < active_count; i++) {
    if (active_games[i].pid == pid) {
      active_games[i] = active_games[active_count - 1];
      active_count--;
      return;
    }
  }
}

/*
 * This function reaps zombie child processes created by any forks in
 * our main function.
 */
void reap_children(void) {
  int status;
  pid_t pid;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    remove_child_game(pid);
  }
}

/*
 * This function performs the handshake with the client connecting to
 * our server. It receives the messages from the TCP stream, validates
 * the initial OPEN message, checks the client's name to ensure it has
 * not already been chosen, sends back a WAIT message to acknowledge
 * state to the client, and then allocates and returns a Player struct.
 */
Player *handle_handshake(int client_fd) {
  char buf[128];
  size_t buf_length = 0;

  // loop until a full message is received from the stream or an error
  // is encountered
  for (;;) {
    ssize_t bytes_received = recv(client_fd, buf + buf_length, sizeof(buf) - buf_length, 0);

    if (bytes_received <= 0) {
      printf("Client disconnected during handshake.\n");
      return NULL;
    }

    buf_length += bytes_received;

    Message message;
    size_t used;
    int return_code = parse_message(buf, buf_length, &message, &used);

    // incomplete return code from parse_message (0)
    if (return_code == 0) {
      continue;
    }

    // failing return code from parse_message (-1)
    if (return_code < 0) {
      send_fail_message(client_fd, "10 Invalid");
      close(client_fd);
      return NULL;
    }

    // we have a complete message stored in message, so remove the
    // message from the buffer
    memmove(buf, buf + used, buf_length - used);
    buf_length -= used;

    // process the OPEN message
    if (strcmp(message.type, "OPEN") != 0) {
      if (strcmp(message.type, "MOVE") == 0) {
        send_fail_message(client_fd, "24 Not Playing");
      } else {
        send_fail_message(client_fd, "10 Invalid");
      }
      close(client_fd);
      return NULL;
    }

    // exactly one field is expected
    if (message.field_count != 1) {
      send_fail_message(client_fd, "10 Invalid");
      close(client_fd);
      return NULL;
    }

    char *name = message.fields[0];

    // validate name
    if (strlen(name) > MAX_NAME_LENGTH) {
      send_fail_message(client_fd, "21 Long Name");
      close(client_fd);
      return NULL;
    }

    if (strchr(name, '|')) {
      send_fail_message(client_fd, "10 Invalid");
      close(client_fd);
      return NULL;
    }

    // check if name is already taken
    if (name_in_active_game(name)) {
      send_fail_message(client_fd, "22 Already Playing");
      close(client_fd);
      return NULL;
    }

    send_wait_message(client_fd);

    // allocate and populate Player struct
    Player *player = malloc(sizeof(Player));
    if (!player) {
      perror("Error while allocating memory for a Player struct");
      close(client_fd);
      return NULL;
    }

    player->fd = client_fd;
    strncpy(player->name, name, MAX_NAME_LENGTH);
    player->name[MAX_NAME_LENGTH] = '\0';
    return player;
  }
}

/*
 * This is the main function that contains the general server logic
 * aside from some parsing logic for the command-line arguments sent
 * by the user.
 */
int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "%s accepts a port number as its one argument.\n", argv[0]);
    return EXIT_FAILURE;
  }

  // require sane port numbers
  int port = atoi(argv[1]);
  if (port <= 0 || port > 65535) {
    fprintf(stderr, "The range of valid port numbers is 1 to 65535.\n");
    return EXIT_FAILURE;
  }

  int listen_fd = create_listen_socket(port);
  if (listen_fd < 0) {
    fprintf(stderr, "Failed to set up listening socket.\n");
    return EXIT_FAILURE;
  }

  printf("Now listening on port %d.\n", port);

  for (;;) {
    // ensure any completed child games are reaped
    reap_children();

    int client_fd = accept_client(listen_fd);
    if (client_fd < 0) {
      continue;
    }

    // ensure child games are reaped again in case of mid-game forfeit
    reap_children();

    Player *player = handle_handshake(client_fd);
    if (!player) {
      printf("Bad handshake, closing client.\n");
      close(client_fd);
      continue;
    }

    printf("Handshake successful for %s (fd %d).\n", player->name, player->fd);

    if (waiting_player == NULL) {
      waiting_player = player;
      printf("%s is waiting for an opponent.\n", player->name);
    } else {
      Player *player1 = waiting_player;
      Player *player2 = player;

      waiting_player = NULL;

      printf("Matching players: %s with %s.\n", player1->name, player2->name);

      pid_t pid = fork();
      if (pid < 0) {
        perror("Error while trying to create a child process");
        send_fail_message(player1->fd, "10 Invalid");
        send_fail_message(player2->fd, "10 Invalid");

        close(player1->fd);
        close(player2->fd);
        free(player1);
        free(player2);
        continue;
      }

      // child process
      if (pid == 0) {
        close(listen_fd);

        if (send_name_message(player1->fd, 1, player2->name) < 0 ||
            send_name_message(player2->fd, 2, player1->name) < 0) {
          printf("[PID %d] Failed to send NAME messages. Not considering for matching.\n", (int)getpid());
          close(player1->fd);
          close(player2->fd);
          free(player1);
          free(player2);
          _exit(1);
        }

        // set up game data to be passed into the game loop
        Game game;
        game.player1 = player1;
        game.player2 = player2;
        game.turn = 1;
        game.board[0] = 1;
        game.board[1] = 3;
        game.board[2] = 5;
        game.board[3] = 7;
        game.board[4] = 9;

        printf("[PID %d] START: %s (P1) vs %s (P2), next player is P%d.\n",
               (int)getpid(), game.player1->name, game.player2->name, game.turn);

        if (send_play_message(player1->fd, game.turn, game.board) < 0 ||
            send_play_message(player2->fd, game.turn, game.board) < 0) {
          printf("[PID %d] Failed to send PLAY messages. Not considering for matching.\n", (int)getpid());
          close(player1->fd);
          close(player2->fd);
          free(player1);
          free(player2);
          _exit(1);
        }

        run_game_loop(&game);

        printf("[PID %d] FINISH: %s (P1) vs %s (P2).\n",
               (int)getpid(), game.player1->name, game.player2->name);

        close(player1->fd);
        close(player2->fd);
        free(player1);
        free(player2);

        _exit(0);
      } else {
        // register game, keep accepting clients
        register_child_game(pid, player1->name, player2->name);

        printf("Spawned game process %d for %s (P1) vs %s (P2).\n",
               (int)pid, player1->name, player2->name);

        close(player1->fd);
        close(player2->fd);
        free(player1);
        free(player2);
      }
    }
  }

  close(listen_fd);
  return EXIT_SUCCESS;
}
