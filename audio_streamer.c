#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <alsa/asoundlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>

#define PORT 8080
#define DEVICE "hw:0,0"
#define RATE 48000
#define CHANNELS 1
#define FORMAT SND_PCM_FORMAT_S16_LE
#define BUFFER_SIZE 4096

void daemonize() {
    pid_t pid;

    // Fork off the parent process
    pid = fork();

    // An error occurred
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    // Success: Let the parent terminate
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // On success: The child process becomes session leader
    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    // Fork off for the second time
    pid = fork();

    // An error occurred
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    // Success: Let the parent terminate
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // Change the file mode mask
    umask(0);

    // Change the current working directory to a safe place
    chdir("/");

    // Close all open file descriptors
    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }
}


void write_wav_header(int fd) {
    char header[44];
    unsigned int sample_rate = RATE;
    unsigned short num_channels = CHANNELS;
    unsigned short bits_per_sample = 16;

    // RIFF chunk
    memcpy(header, "RIFF", 4);
    *(unsigned int *)(header + 4) = 0xFFFFFFFF; // Chunk size (unknown for streaming)
    memcpy(header + 8, "WAVE", 4);

    // fmt sub-chunk
    memcpy(header + 12, "fmt ", 4);
    *(unsigned int *)(header + 16) = 16; // Sub-chunk size for PCM
    *(unsigned short *)(header + 20) = 1; // Audio format (1 for PCM)
    *(unsigned short *)(header + 22) = num_channels;
    *(unsigned int *)(header + 24) = sample_rate;
    *(unsigned int *)(header + 28) = sample_rate * num_channels * bits_per_sample / 8; // Byte rate
    *(unsigned short *)(header + 32) = num_channels * bits_per_sample / 8; // Block align
    *(unsigned short *)(header + 34) = bits_per_sample;

    // data sub-chunk
    memcpy(header + 36, "data", 4);
    *(unsigned int *)(header + 40) = 0xFFFFFFFF; // Sub-chunk size (unknown for streaming)

    if (write(fd, header, 44) != 44) {
        // Cannot write to stderr because it's closed in daemon mode
    }
}

void audio_capture_process(int pipe_write_fd) {
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *params;
    int err;
    unsigned int rate = RATE;
    char buffer[BUFFER_SIZE];

    // ALSA setup
    if ((err = snd_pcm_open(&handle, DEVICE, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        exit(1);
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);
    snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, params, FORMAT);
    snd_pcm_hw_params_set_channels(handle, params, CHANNELS);
    snd_pcm_hw_params_set_rate_near(handle, params, &rate, 0);

    if ((err = snd_pcm_hw_params(handle, params)) < 0) {
        exit(1);
    }

    int bytes_per_frame = (snd_pcm_format_width(FORMAT) / 8) * CHANNELS;
    int frames_to_read = BUFFER_SIZE / bytes_per_frame;

    while (1) {
        if ((err = snd_pcm_readi(handle, buffer, frames_to_read)) < 0) {
            if (err == -EPIPE) {
                snd_pcm_prepare(handle);
            }
            continue;
        }
        if (write(pipe_write_fd, buffer, err * bytes_per_frame) < 0) {
            break;
        }
    }

    snd_pcm_close(handle);
    close(pipe_write_fd);
    exit(0);
}

int main(int argc, char *argv[]) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);
    int pipe_fd[2];
    pid_t audio_pid;
    int daemon_mode = 0;

    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
        daemonize();
    }


    // Create pipe for audio data
    if (pipe(pipe_fd) == -1) {
        if (!daemon_mode) perror("pipe");
        exit(EXIT_FAILURE);
    }

    // Fork audio capture process
    audio_pid = fork();
    if (audio_pid < 0) {
        if (!daemon_mode) perror("fork");
        exit(EXIT_FAILURE);
    }

    if (audio_pid == 0) { // Child process (audio capture)
        close(pipe_fd[0]); // Close read end
        audio_capture_process(pipe_fd[1]);
        // This process will exit via audio_capture_process
    }

    // Parent process (server)
    close(pipe_fd[1]); // Close write end

    // Socket setup
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        if (!daemon_mode) perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        if (!daemon_mode) perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        if (!daemon_mode) perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 10) < 0) {
        if (!daemon_mode) perror("listen");
        exit(EXIT_FAILURE);
    }

    if (!daemon_mode) {
        printf("Server listening on port %d\n", PORT);
    }


    fd_set read_fds;
    int max_fd;
    int client_sockets[FD_SETSIZE];
    int num_clients = 0;

    for (int i = 0; i < FD_SETSIZE; i++) {
        client_sockets[i] = 0;
    }

    while(1) {
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        FD_SET(pipe_fd[0], &read_fds);
        max_fd = (server_fd > pipe_fd[0]) ? server_fd : pipe_fd[0];

        for (int i = 0; i < num_clients; i++) {
            int sd = client_sockets[i];
            if (sd > 0) {
                FD_SET(sd, &read_fds);
            }
            if (sd > max_fd) {
                max_fd = sd;
            }
        }

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        if ((activity < 0) && (errno != EINTR)) {
            // Cannot use perror in daemon mode
        }

        // New connection
        if (FD_ISSET(server_fd, &read_fds)) {
            int client_socket;
            if ((client_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
                exit(EXIT_FAILURE);
            }

            // Send HTTP header
            char header[] = "HTTP/1.1 200 OK\n"
                            "Content-Type: audio/wav\n"
                            "Connection: close\n"
                            "\n";
            if (write(client_socket, header, strlen(header)) < 0) {
                close(client_socket);
            } else {
                write_wav_header(client_socket);
                 // Add new socket to array of sockets
                for (int i = 0; i < FD_SETSIZE; i++) {
                    if (client_sockets[i] == 0) {
                        client_sockets[i] = client_socket;
                        if (i >= num_clients) {
                            num_clients = i + 1;
                        }
                        break;
                    }
                }
            }
        }

        // Audio data from pipe
        if (FD_ISSET(pipe_fd[0], &read_fds)) {
            char audio_buffer[BUFFER_SIZE];
            ssize_t bytes_read = read(pipe_fd[0], audio_buffer, BUFFER_SIZE);
            if (bytes_read > 0) {
                // Broadcast to all clients
                for (int i = 0; i < num_clients; i++) {
                    int sd = client_sockets[i];
                    if (sd > 0) {
                        if (write(sd, audio_buffer, bytes_read) < 0) {
                            // Error or client disconnected
                            close(sd);
                            client_sockets[i] = 0;
                        }
                    }
                }
            } else {
                // Pipe closed or error
                break; // Exit main loop
            }
        }

        // Check for client disconnects
        for (int i = 0; i < num_clients; i++) {
            int sd = client_sockets[i];
            if (sd > 0 && FD_ISSET(sd, &read_fds)) {
                char disconnect_buffer[1024];
                if (read(sd, disconnect_buffer, sizeof(disconnect_buffer)) == 0) {
                    // Client disconnected
                    close(sd);
                    client_sockets[i] = 0;
                }
            }
        }
    }

    // Clean up
    for (int i = 0; i < num_clients; i++) {
        if(client_sockets[i] > 0) close(client_sockets[i]);
    }
    close(server_fd);
    close(pipe_fd[0]);
    kill(audio_pid, SIGTERM);
    waitpid(audio_pid, NULL, 0);

    return 0;
}