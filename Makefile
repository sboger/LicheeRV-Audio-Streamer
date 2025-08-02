CC=gcc
CFLAGS=-O2 -Wall
LDFLAGS=-lasound -lpthread

all: audio_streamer

audio_streamer: audio_streamer.c
	$(CC) $(CFLAGS) -o audio_streamer audio_streamer.c $(LDFLAGS)
	strip audio_streamer

clean:
	rm -f audio_streamer
