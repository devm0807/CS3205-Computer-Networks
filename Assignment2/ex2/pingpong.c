/**
 * Networked Ping Pong Game
 * CS3205 - Assignment 2 (Holi'25)
 * Instructor: Ayon Chakraborty
 * TA Group 1
 * CS22B007, CS22B008
 **/

#include <ncurses.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define WIDTH 80
#define HEIGHT 30
#define OFFSETX 10
#define OFFSETY 5
#define PADDLE_WIDTH 10
#define UPDATE_INTERVAL 40000 // 40ms

typedef struct {
    int x, y;
    int dx, dy;
} Ball;

typedef struct {
    int x;
} Paddle;

typedef struct {
    Ball ball;
    Paddle paddleA, paddleB;
    int scoreA, scoreB;
} GameState;


GameState prevState;
GameState state;
int game_running = 1;
int is_server = 0;
int sockfd;
int client_sock;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void network_setup(int argc, char *argv[]);
void init_ncurses();
void draw_permanent(WINDOW *win);
void erase_tmp(WINDOW *win);
void draw(WINDOW *win);
void *move_ball(void *arg);
void *server_send(void *arg);
void *server_recv(void *arg);
void *client_recv(void *arg);
void reset_ball();
void end_game();

int main(int argc, char *argv[]) {

    // Game init
    state.ball = (Ball){WIDTH/2, HEIGHT/2, 1, 1};
    state.paddleA = (Paddle){WIDTH/2 - PADDLE_WIDTH/2};
    state.paddleB = (Paddle){WIDTH/2 - PADDLE_WIDTH/2};
    state.scoreA = 0;
    state.scoreB = 0;

    // invalid values initially
    prevState.ball = (Ball){-1,-1,0,0};
    prevState.paddleA = (Paddle){-1};
    prevState.paddleB = (Paddle){-1};
    prevState.scoreA = -1;
    prevState.scoreB = -1;

    network_setup(argc, argv);
    
    init_ncurses();

    draw_permanent(stdscr);
    
    if(is_server){
        pthread_t ball_thread, send_thread, recv_thread;
        pthread_create(&ball_thread, NULL, move_ball, NULL);
        pthread_create(&send_thread, NULL, server_send, NULL);
        pthread_create(&recv_thread, NULL, server_recv, NULL);

        while(game_running) {
            int ch = getch();
            if(ch == 'q'){
                game_running = 0;
                break;
            }
            
            pthread_mutex_lock(&mutex);
            if(ch == KEY_LEFT && state.paddleA.x > 1) state.paddleA.x-=2;
            if(ch == KEY_RIGHT && state.paddleA.x < WIDTH-PADDLE_WIDTH-1) state.paddleA.x+=2;
            pthread_mutex_unlock(&mutex);
            
            pthread_mutex_lock(&mutex);
            draw(stdscr);
            pthread_mutex_unlock(&mutex);
        }

        close(client_sock);
        pthread_join(ball_thread, NULL);
        pthread_join(send_thread, NULL);
        pthread_join(recv_thread, NULL);

    } else {
        pthread_t recv_thread;
        pthread_create(&recv_thread, NULL, client_recv, NULL);

        while(game_running) {
            int ch = getch();
            if(ch == 'q') game_running = 0;
            
            int new_x = state.paddleB.x;


            if(ch == KEY_LEFT && state.paddleB.x > 1) state.paddleB.x-=2;
            if(ch == KEY_RIGHT && state.paddleB.x < WIDTH-PADDLE_WIDTH-1) state.paddleB.x+=2;
            
            if(new_x != state.paddleB.x) {
                int paddleB_x = htons(state.paddleB.x);
                send(sockfd, &paddleB_x, sizeof(paddleB_x), 0);
            }
            
            pthread_mutex_lock(&mutex);
            draw(stdscr);
            pthread_mutex_unlock(&mutex);
        }

        pthread_join(recv_thread, NULL);
    }
    close(sockfd);
    end_game();
    return 0;
}

void network_setup(int argc, char *argv[]) {
    if(argc < 2) {
        fprintf(stderr, "Usage: %s server <PORT>\n      %s client <SERVER_IP>\n", argv[0], argv[0]);
        exit(1);
    }
    if(strcmp(argv[1], "server") == 0) {
        is_server = 1;
        // Check if the server port is provided
        if(argc != 3) {
            fprintf(stderr, "Usage: %s server <PORT>\n", argv[0]);
            exit(1);
        }
        // Network setup
        int port = atoi(argv[2]);
        sockfd = socket(AF_INET, SOCK_STREAM, 0);   // TCP socket creation
        if(sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }
        struct sockaddr_in server_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr.s_addr = INADDR_ANY
        };

        if(bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr))<0) {
            perror("Bind failed");
            close(sockfd);
            exit(1);
        }
        if(listen(sockfd, 1) < 0) {
            perror("Listen failed");
            close(sockfd);
            exit(1);
        }

        printf("Server listening on port %d...\n", port);
        client_sock = accept(sockfd, NULL, NULL);
        if(client_sock < 0) {
            perror("Accept failed");
            close(sockfd);
            exit(1);
        }
        printf("Client connected.\n");

    } else if(strcmp(argv[1], "client") == 0) {
        is_server = 0;
        // Check if the server IP is provided
        if(argc != 3) {
            fprintf(stderr, "Usage: %s client <SERVER_IP>\n", argv[0]);
            exit(1);
        }

        // Network setup
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if(sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }
        struct sockaddr_in client_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(8080),
            .sin_addr.s_addr = inet_addr(argv[2])
        };
        if(connect(sockfd, (struct sockaddr*)&client_addr, sizeof(client_addr))<0) {
            perror("Connection failed");
            close(sockfd);
            exit(1);
        }
        printf("Connected to server %s:8080.\n", argv[2]);
    } else {
        fprintf(stderr, "Invalid argument. Use 'server' or 'client'.\n");
        exit(1);
    }
}

void init_ncurses() {
    initscr();
    start_color();

    // Create (foreground, background) color pairs
    init_pair(1, COLOR_BLUE, COLOR_WHITE);  // Color pair for the Scores
    init_pair(2, COLOR_YELLOW, COLOR_YELLOW);   // Color pair for the bottom paddle
    init_pair(3, COLOR_GREEN, COLOR_GREEN); // Color pair for the top paddle
    init_pair(4, COLOR_RED, COLOR_RED); // Color pair for the ball
    init_pair(5, COLOR_CYAN, COLOR_CYAN); // Color pair for the net and borders

    timeout(10);    // timeout till a getch
    keypad(stdscr, TRUE);   // for using arrow keys
    curs_set(FALSE);    // Hide cursor
    noecho();   // Don't echo user input
}

void draw_permanent(WINDOW *win){
    clear();

    // Draw Title
    attron(COLOR_PAIR(1));
    if(is_server) mvprintw(OFFSETY-1, OFFSETX, "CS3205 NetPong (Server)");
    else mvprintw(OFFSETY-1, OFFSETX, "CS3205 NetPong (Client)");
    attroff(COLOR_PAIR(1));

    // Draw borders
    attron(COLOR_PAIR(5));
    for(int i=OFFSETX; i<OFFSETX+WIDTH; i++) {
        mvprintw(OFFSETY, i, " ");
        mvprintw(OFFSETY+HEIGHT-1, i, " ");
    }
    for(int i=OFFSETY; i<OFFSETY+HEIGHT; i++) {
        mvprintw(i, OFFSETX, " ");
        mvprintw(i, OFFSETX+WIDTH-1, " ");
    }
    // Draw the net
    for(int i=OFFSETX+1; i<OFFSETX+WIDTH-1; i+=2) {
        mvprintw(OFFSETY+HEIGHT/2, i, "-");
    }
    attroff(COLOR_PAIR(5));
    refresh();   
}

void erase_tmp(WINDOW *win){
    // erase old ball
    mvprintw(OFFSETY + prevState.ball.y, OFFSETX + prevState.ball.x, " ");

    // erase old paddle A
    for(int i=0; i<PADDLE_WIDTH; i++) { // Bottom paddle
        mvprintw(OFFSETY + HEIGHT -4, OFFSETX + prevState.paddleA.x + i, " ");
    }

    // erase old paddle B
    for(int i=0; i<PADDLE_WIDTH; i++) { // Top paddle
        mvprintw(OFFSETY + 3, OFFSETX + prevState.paddleB.x + i, " ");
    }

    // erase old scores
    mvprintw(OFFSETY-1, OFFSETX+WIDTH-20, "                               ");
}

void draw(WINDOW *win) {
    erase_tmp(win);

    // Draw score panel
    attron(COLOR_PAIR(1));
    mvprintw(OFFSETY-1, OFFSETX+WIDTH-27, "Player A: %d  Player B: %d", state.scoreA, state.scoreB);
    attroff(COLOR_PAIR(1));
    
    // Draw ball
    attron(COLOR_PAIR(4));
    mvprintw(OFFSETY + state.ball.y, OFFSETX + state.ball.x, "o");
    attroff(COLOR_PAIR(4));
    
    // Draw paddles
    attron(COLOR_PAIR(2));
    for(int i=0; i<PADDLE_WIDTH; i++) { // Bottom paddle
        mvprintw(OFFSETY + HEIGHT -4, OFFSETX + state.paddleA.x + i, " ");
    }
    attroff(COLOR_PAIR(2));

    attron(COLOR_PAIR(3));
    for(int i=0; i<PADDLE_WIDTH; i++) { // Top paddle
        mvprintw(OFFSETY + 3, OFFSETX + state.paddleB.x + i, " ");
    }
    attroff(COLOR_PAIR(3));
    
    prevState = state;
    refresh();
}

void *move_ball(void *arg) {
    while(game_running) {
        pthread_mutex_lock(&mutex);
        
        state.ball.x += state.ball.dx;
        state.ball.y += state.ball.dy;

        // Top paddle collision (paddleB)
        if(state.ball.y == 3 && state.ball.x >= state.paddleB.x && 
           state.ball.x < state.paddleB.x + PADDLE_WIDTH) {
            state.ball.dy = -state.ball.dy;
        }

        // Bottom paddle collision (paddleA)
        if(state.ball.y == HEIGHT-5 && state.ball.x >= state.paddleA.x && 
           state.ball.x < state.paddleA.x + PADDLE_WIDTH) {
            state.ball.dy = -state.ball.dy;
        }

        // Score handling
        if(state.ball.y <= 1) { state.scoreA++; reset_ball(); }
        if(state.ball.y >= HEIGHT-3) { state.scoreB++; reset_ball(); }

        // Wall collisions
        if(state.ball.x <= 2 || state.ball.x >= WIDTH-2) state.ball.dx = -state.ball.dx;

        pthread_mutex_unlock(&mutex);
        usleep(100000);  // slow down the ball
    }
    return NULL;
}

void *server_send(void *arg) {
    GameState send_state;
    while(game_running) {
        pthread_mutex_lock(&mutex);
        send_state.ball.x = htons(state.ball.x);
        send_state.ball.y = htons(state.ball.y);
        send_state.ball.dx = htons(state.ball.dx);
        send_state.ball.dy = htons(state.ball.dy);
        send_state.paddleA.x = htons(state.paddleA.x);
        send_state.scoreA = htons(state.scoreA);
        send_state.scoreB = htons(state.scoreB);
        pthread_mutex_unlock(&mutex);
        
        send(client_sock, &send_state, sizeof(send_state), 0);
        usleep(UPDATE_INTERVAL);
    }
    return NULL;
}

void *server_recv(void *arg) {
    int tmp;
    while(game_running) {
        if(recv(client_sock, &tmp, sizeof(tmp), 0) > 0) {
            pthread_mutex_lock(&mutex);
            state.paddleB.x = ntohs(tmp);
            pthread_mutex_unlock(&mutex);
        }
    }
    return NULL;
}

void *client_recv(void *arg) {
    GameState recv_state;
    while(game_running) {
        if(recv(sockfd, &recv_state, sizeof(recv_state), 0) > 0) {
            pthread_mutex_lock(&mutex);
            state.ball.x = ntohs(recv_state.ball.x);
            state.ball.y = ntohs(recv_state.ball.y);
            state.ball.dx = ntohs(recv_state.ball.dx);
            state.ball.dy = ntohs(recv_state.ball.dy);
            state.paddleA.x = ntohs(recv_state.paddleA.x);
            state.scoreA = ntohs(recv_state.scoreA);
            state.scoreB = ntohs(recv_state.scoreB);
            pthread_mutex_unlock(&mutex);
        }
    }
    return NULL;
}

// void *client_send(void *arg) {
//     while(game_running) {
//         pthread_mutex_lock(&mutex);
//         send(sockfd, &paddleB.x, sizeof(paddleB.x), 0);
//         pthread_mutex_unlock(&mutex);
//         usleep(UPDATE_INTERVAL);
//     }
//     return NULL;
// }

void reset_ball() {
    state.ball.x = WIDTH/2;
    state.ball.y = HEIGHT/2;
    // from middle, along any diagonal direction
    state.ball.dx = ((rand() % 2) == 0) ? 1 : -1;
    state.ball.dy = ((rand() % 2) == 0) ? 1 : -1;
}


void end_game() {
    endwin();
}
