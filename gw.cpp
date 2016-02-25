#include <cstdlib>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

#include <sys/stat.h>
#include <pwd.h>
#include <syslog.h>
#include <stdarg.h>
#include <string> 
#include <RF24/RF24.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define PORT    5004
#define MAXMSG  512
pthread_t thread_id;

int sock;
fd_set active_fd_set, read_fd_set;

/* variable indicating if the server is still running */
volatile static int running = 1;
char pong[] = "Yap ... i'm here...\n";
int daemonizeFlag = 0;
short msg_count;
typedef unsigned char byte;

// Radio pipe addresses for the 2 nodes to communicate.
// First pipe is for writing, 2nd, 3rd, 4th, 5th & 6th is for reading...
#define StringLen 21		//Длина строк в структурах в сумме с MaxRelayOnNet не должно превышать 20

#pragma pack(4)
struct itank_t {
	byte ver;
	byte node_req;				//1
	byte node_to;				//1
	short pack_req;				//2
	byte fragment;
	byte ack;
	byte repited;
	short value;           		//2
	byte pin;                   //1
	byte name[StringLen];		//
}__attribute__((__packed__));
itank_t package;

/*
 *  byte ver;                     //1
 byte node_req;                //1
 byte node_to;                 //1
 int  pack_req;                //2 байта
 byte fragment;                //1
 byte ack;                     //1
 byte repited;                 //1
 int  value;                   //2
 byte pin;                     //1
 byte name[StringLen];         //21*/

typedef unsigned char byte;
using namespace std;

// Radio pipe addresses for the 2 nodes to communicate.
// First pipe is for writing, 2nd, 3rd, 4th, 5th & 6th is for reading...
const uint64_t pipes[2] = { 0xDEADBEEF00LL, 0xDEADBEEFF4LL };

// Setup for GPIO 22 CE and CE1 CSN with SPI Speed @ 8Mhz
RF24 radio(RPI_V2_GPIO_P1_15, BCM2835_SPI_CS0, BCM2835_SPI_SPEED_8MHZ);

void openSyslog() {
	setlogmask (LOG_UPTO(LOG_INFO));openlog(NULL, 0, LOG_USER);
}

void closeSyslog() {
	closelog();
}

void log(int priority, const char *format, ...) {
	va_list argptr;
	va_start(argptr, format);
	if (daemonizeFlag == 1) {
		vsyslog(priority, format, argptr);
	} else {
		vprintf(format, argptr);
	}
	va_end(argptr);
}

/*
 * handler for SIGINT signal
 */
void handle_sigint(int sig) {
	log(LOG_INFO, "Received SIGINT\n");
	running = 0;
}

void handle_sigusr1(int sig) {

	log(LOG_INFO, "Received SIGUSR1\n");
	running = 0;

	int curLogLevel = setlogmask(0);
	if (curLogLevel != LOG_UPTO(LOG_DEBUG))
		setlogmask (LOG_UPTO(LOG_DEBUG));else setlogmask(LOG_UPTO (LOG_INFO));
	}

	/*
	 void trap_zabbix(itank_t package) {
	 char line[150];
	 snprintf(line, 150, "/root/scripts/trap.pl %i %i %i %i %s &",
	 package.pack_req, package.node_to, package.node_req, package.value,
	 package.com);
	 //	printf(line);
	 //	system(line);
	 }
	 */
void radio_init(void) {
	radio.begin();
//	radio.enableDynamicPayloads();
	radio.setAutoAck(1);
	radio.setRetries(15, 15);
	radio.setDataRate(RF24_250KBPS);
	radio.setPALevel(RF24_PA_MAX);
	radio.setChannel(76);
//	radio.setCRCLength(RF24_CRC_8);
	radio.openWritingPipe(pipes[0]);
	radio.openReadingPipe(1, pipes[1]);
	radio.openReadingPipe(2, pipes[2]);
	radio.openReadingPipe(3, pipes[3]);
	radio.openReadingPipe(4, pipes[4]);
	radio.openReadingPipe(5, pipes[5]);
	radio.startListening();
	radio.printDetails();
	delay(1);

}

void CharToByte(char* chars, byte* bytes, unsigned int count) {
	for (unsigned int i = 0; i < count; i++)
		bytes[i] = (byte) chars[i];
}

void ByteToChar(byte* bytes, char* chars, unsigned int count) {
	for (unsigned int i = 0; i < count; i++)
		chars[i] = (char) bytes[i];
}

void send(itank_t package) {
	if (msg_count == 32767)
		msg_count = 0;
	else
		msg_count++;
	package.pack_req = msg_count;
	package.fragment=0;
	package.repited=0;
	package.node_req=0;
	log(LOG_INFO,
			"1 Подготовлен пакет для ноды %d с номером %d  pin=%i '%s'='%d' ...\n",
			package.node_req, package.pack_req, package.pin, package.name,
			package.value);
	radio.stopListening();
	log(LOG_INFO,
			"2 Подготовлен пакет для ноды %d с номером %d  pin=%i '%s'='%d' ...\n",
			package.node_req, package.pack_req, package.pin, package.name,
			package.value);

	radio.write(&package, sizeof(package));
// Now, resume listening so we catch the next packets.
	radio.startListening();
// Spew it
	log(LOG_INFO,
			"Отправлен пакет для ноды %d с номером %d  pin=%i '%s'='%d' ...\n",
			package.node_req, package.pack_req, package.pin, package.name,
			package.value);
	delay(925); //Delay after payload responded to, minimize RPi CPU time

}

void parseAndSend(char *commandBuffer) {
	char *str, *p;
	itank_t to_send;
	int i = 0;
	to_send.pin = 0;
// printf(commandBuffer);
	for (str = strtok_r(commandBuffer, ";", &p);       // split using semicolon
			str && i <= 5;         // loop while str is not null an max 5 times
			str = strtok_r(NULL, ";", &p)               // get subsequent tokens
					) {
		switch (i) {
		case 0: // ver
			to_send.ver = (unsigned short) strtoul(str, NULL, 0);
			log(LOG_INFO, "ver: %i ", to_send.ver);
			break;
		case 1: // Childid
			to_send.node_to = (unsigned short) strtoul(str, NULL, 0);
			log(LOG_INFO, "node_to: %i ", to_send.node_to);
			break;
		case 2: // ACK
			to_send.ack = (unsigned short) strtoul(str, NULL, 0);
			log(LOG_INFO, "ack: %i ", to_send.ack);
			break;
		case 3: // value
			to_send.value = (short) strtoul(str, NULL, 0);
			log(LOG_INFO, "VALUE: %i ", to_send.value);
			break;

		case 4: // value
			to_send.pin = (unsigned short) strtoul(str, NULL, 0);
			log(LOG_INFO, "Pin: %i ", to_send.pin);
			break;
		case 5: // text
			CharToByte(str, to_send.name, StringLen);
			//to_send.com = (unsigned char) str;
			log(LOG_INFO, "COMMENT: %s \n", to_send.name);
			break;
//			1;47;2;3;4;SPinStatus;
		}
		i++;
	}
	if( i < 5 )return ;
	send(to_send);

}

void *connection_nrf(void *running);
void *connection_nrf(void *running) {
	while (running) {
		// Start listening
		while (radio.available()) {
			radio.read(&package, sizeof(package));
			if (package.ver != 1)
				continue;
			char line[200];
			snprintf(line, 200,
					"{\"ver\":\"%i\",\"node_req\":\"%i\",\"pack_req\":\"%i\", \"node_to\":\"%i\",\"fragment\":\"%i\",\"ack\":\"%i\",\"repited\":\"%i\",\"value\":\"%i\",\"pin\":\"%i\",\"name\":\"%s\" } \n",
					package.ver, package.node_req, package.pack_req,
					package.node_to, package.fragment, package.ack,
					package.repited, package.value, package.pin, package.name);
			for (int i = 0; i < FD_SETSIZE; ++i)
				if (FD_ISSET(i, &read_fd_set))
					if (i != sock)
						write(i, line, strlen(line));
			strtok(line, "\r\n");
			log(LOG_INFO, "[TCPServer] send: '%s'\n", line);
			/*
			 *  byte ver;                     //1
			 byte node_req;                //1
			 byte node_to;                 //1
			 int  pack_req;                //2 байта
			 byte fragment;                //1
			 byte ack;                     //1
			 byte repited;                 //1
			 int  value;                   //2
			 byte pin;                     //1
			 byte name[StringLen];         //21*/
//		trap_zabbix(package);
		};
		delay(10);
	}
	return 0;
}
;
int read_from_client(int filedes) {
	char buffer[MAXMSG];
	char _buffer[MAXMSG];

	int nbytes;

	nbytes = read(filedes, buffer, MAXMSG);
	if (nbytes < 0) {
		/* Read error. */
		perror("read");
		exit (EXIT_FAILURE);
	} else if (nbytes == 0)
		/* End-of-file. */
		return -1;
	else {
		memcpy(_buffer, buffer, nbytes);
		strtok(_buffer, "\r\n");
		log(LOG_INFO, "[TCPServer] receive: '%s'\n", _buffer);
		if (strcmp(_buffer, "Any body home?") == 0) {
			log(LOG_INFO, "[TCPServer] ping...pong\n");
			write(filedes, pong, sizeof(pong));
			return 0;
		};
		/* Data read. */
		buffer[nbytes] = '\0';

		parseAndSend(buffer);

		return 0;
	}
}

int main(int argc, char** argv) {
// Refer to RF24.h or nRF24L01DS for settings
	int status = EXIT_SUCCESS;
	int c;

	while ((c = getopt(argc, argv, "d")) != -1) {
		switch (c) {
		case 'd':
			daemonizeFlag = 1;
			break;
		}
	}
	openSyslog();
	printf("Размер пакета для обмена %d byte \n", sizeof(package));
	/* register the signal handler */
	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigint);
	signal(SIGUSR1, handle_sigusr1);
	/* tcp */
	extern int make_socket(uint16_t port);

	int i;
	struct sockaddr_in clientname;
	size_t size;

	/* Create the socket and set it up to accept connections. */
	sock = make_socket(PORT);
	if (listen(sock, 1) < 0) {
		perror("listen");
		exit (EXIT_FAILURE);
	}
	log(LOG_INFO, "[TCPServer] TCPListen  0.0.0.0:%i\n", PORT);

	/* Initialize the set of active sockets. */
	FD_ZERO(&active_fd_set);
	FD_SET(sock, &active_fd_set);

	radio_init();
	if (pthread_create(&thread_id, NULL, connection_nrf, (void*) &running)
			< 0) {
		perror("could not create thread");
		return 1;
	}
	while (running) { /* Block until input arrives on one or more active sockets. */

		read_fd_set = active_fd_set;
		if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL) < 0) {
			perror("select");
			exit (EXIT_FAILURE);
		}

		/* Service all the sockets with input pending. */
		for (i = 0; i < FD_SETSIZE; ++i)
			if (FD_ISSET(i, &read_fd_set)) {
				if (i == sock) {
					/* Connection request on original socket. */
					int new_connect;
					size = sizeof(clientname);
					new_connect = accept(sock, (struct sockaddr *) &clientname,
							(socklen_t*) &size);
					if (new_connect < 0) {
						perror("accept");
						exit (EXIT_FAILURE);
					}
					log(LOG_INFO,
							"[TCPServer] connect from host %s, port %hd \n",
							inet_ntoa(clientname.sin_addr),
							ntohs(clientname.sin_port));
					FD_SET(new_connect, &active_fd_set);
				} else {
					/* Data arriving on an already-connected socket. */
					if (read_from_client(i) < 0) {
						close(i);
						FD_CLR(i, &active_fd_set);
					}
				}
			}

	}
	log(LOG_INFO, "Exiting...\n");
	closeSyslog();
	return status;

}

int make_socket(uint16_t port) {
	int sock;
	struct sockaddr_in name;

	/* Create the socket. */
	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		exit (EXIT_FAILURE);
	}

	/* Give the socket a name. */
	name.sin_family = AF_INET;
	name.sin_port = htons(port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);
	int opt = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
		perror("setsockopt");
	if (bind(sock, (struct sockaddr *) &name, sizeof(name)) < 0) {
		perror("bind");
		exit (EXIT_FAILURE);
	}

	return sock;
}

