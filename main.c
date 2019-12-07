#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <arpa/inet.h> 
#include <netinet/in.h> 
#include <sys/types.h> 
#include <netdb.h>

#define CAPTEUR_I2C_NAME			"/dev/i2c-2"
#define CAPTEUR_CTRL_REG1_A			0x20
#define CAPTEUR_CTRL_REG1_A_VALUE	0x57
#define CAPTEUR_READ_ADDR			0xA8 //C'est 0x28 + le bit le plus fort à 1 pour dire que l'on veut une lecture continue

#define UDP_SERVER_ADDR				"10.10.2.62"
#define UDP_SERVER_PORT				20000
#define UDP_SERVER_BUFFER_SIZE		1024
#define UDP_CLIENT_ADDR				"192.168.1.100"

#define TCP_SERVER_ADDR				"10.10.2.62"
#define TCP_SERVER_PORT				21000
#define TCP_SERVER_BUFFER_SIZE		1024
#define TCP_CLIENT_ADDR				"192.168.1.100"

#define PORT_UBIDOTS 80


typedef struct
{
	float X;
	float Y;
	float Z;
} AccValues_t;

int fileId;
char buffer[2];//Buffer utiliser pour communiquer avec le capteur
AccValues_t values;
/*
 Erreur étrange, l'utilisation de la fonction d'initialisation classique du mutex ne semble pas fonctionner
Nous utilisons donc la constante PTHREAD_MUTEX_INITIALIZER qui initialise le mutex de façon "rapide"
*/
pthread_mutex_t threadLock = PTHREAD_MUTEX_INITIALIZER;// Mutex utilisé pour éviter que plusieurs thread utilisent les variables en même temps


float formatValue(short val) {
	float formatedValue = (float)val;
	formatedValue /= 16384;
	return formatedValue;
}

void openAndConfigureI2C() {
	
	unsigned char i2cDeviceAddr = 0b0011001;
	
	//Dans un premier temps on va ouvrir le module i2c
	fileId = open(CAPTEUR_I2C_NAME, O_RDWR);
	if(fileId < 0) {
		printf("Impossible d'ouvrir le module I2C\n");
		exit(1);
	}
	
	//Puis on va configurer l'addresse du capteur
	if (ioctl(fileId, I2C_SLAVE, i2cDeviceAddr) < 0) {
    		printf("Impossible de configurer l'adresse du capteur\n");
    		exit(1);
	}
	
	//On va configurer les registres internes du capteur pour activer XYZ
	buffer[0] = CAPTEUR_CTRL_REG1_A;
	buffer[1] = CAPTEUR_CTRL_REG1_A_VALUE;
	if (write(fileId, buffer, 2) != 2) {
		printf("Impossible d'écrire dans le capteur\n");
		exit(1);
	}
}

void readValues(AccValues_t *values) {
	short valX,valY,valZ;
	buffer[0] = CAPTEUR_READ_ADDR;
	if (write(fileId, buffer, 1) != 1) {
		printf("Impossible d'écrire le registre à lire\n");
		exit(1);
	}
	
	if(read(fileId, buffer, 6) != 6) {
		printf("Impossible de lire les registres\n");
		exit(1);
	}
	
	valX = buffer[1] << 8 | buffer[0];
	valY = buffer[3] << 8 | buffer[2];
	valZ = buffer[5] << 8 | buffer[4];
	
	pthread_mutex_lock(&threadLock);
	values->X = formatValue(valX);
	values->Y = formatValue(valY);
	values->Z = formatValue(valZ);
	pthread_mutex_unlock(&threadLock);
}

void* readThread(void* arg) {
	openAndConfigureI2C();
	while (1) {
		readValues(&values);
		usleep(100000);
	}
}

void* displayThread(void* arg) {
	float displayValueX, displayValueY, displayValueZ;
	while(1) {
		/* Zone de code critique, on va copier les valeurs dans des variables locales pour 
		   ne pas passer trop de temps de la section critique */
		pthread_mutex_lock(&threadLock);
		displayValueX = values.X;
		displayValueY = values.Y;
		displayValueZ = values.Z;
		pthread_mutex_unlock(&threadLock);

		printf("\e[1;1H");
		printf("X: %f\n",displayValueX);
		printf("Y: %f\n",displayValueY);
		printf("Z: %f\n\n",displayValueZ);
		usleep(100000);
	}	
}

/*******************
*
* UDP 
*
********************/

void sendToUDPServer(int sockfd, struct sockaddr_in servaddr) {
	float displayValueX, displayValueY, displayValueZ;
	char buffer[UDP_SERVER_BUFFER_SIZE];
	int n, len;

	pthread_mutex_lock(&threadLock);
	displayValueX = values.X;
	displayValueY = values.Y;
	displayValueZ = values.Z;
	pthread_mutex_unlock(&threadLock);

	sprintf(buffer, "x: %f Y: %f Z:%f", displayValueX, displayValueY, displayValueZ);
// Envoyer au socket

	sendto(sockfd, buffer, strlen(buffer), 0, (const struct sockaddr *) &servaddr, sizeof(servaddr)); 
	
// Recevoir sur le socket

	n = recvfrom(sockfd, buffer, 1024, 0, (struct sockaddr *) &servaddr, &len); 
	buffer[n] = '\0';

	//printf("\e[5;1H");
    printf("Msg from server : %s\n", buffer); 
  
}

void* updSendThread(void* args) {
	struct sockaddr_in   socketAddrinfo;
	int sock; 
	 
	memset(&socketAddrinfo, 0, sizeof(socketAddrinfo)); 
	
	sock = socket(AF_INET,  SOCK_DGRAM, 0);// Creation du Socket

	// Infos client
	struct sockaddr_in localaddr = {0};
	localaddr.sin_family = AF_INET;
	localaddr.sin_addr.s_addr = inet_addr(UDP_CLIENT_ADDR);
	if(bind(sock, (struct sockaddr*)&localaddr, sizeof(struct sockaddr_in)) == 0) {
		printf("UDP bind success\n");
	} else {
		printf("UDP unable to bind\n");
	}

	// Infos serveur 
	socketAddrinfo.sin_family = AF_INET; 
	socketAddrinfo.sin_port = htons(UDP_SERVER_PORT);	
	socketAddrinfo.sin_addr.s_addr = inet_addr(UDP_SERVER_ADDR);

	while(1) {
		sendToUDPServer(sock, socketAddrinfo);
		sleep(1);
	}
	
}

/*******************
*
* TCP 
*
********************/

void sendToTCPServer(int sock){
	float displayValueX, displayValueY, displayValueZ;
	char buffer[UDP_SERVER_BUFFER_SIZE];
	int n;

	pthread_mutex_lock(&threadLock);
	displayValueX = values.X;
	displayValueY = values.Y;
	displayValueZ = values.Z;
	pthread_mutex_unlock(&threadLock);

	//En TCP il faut indiquer au serveur la fin de trame par \r\n (c'est le serveur qui demande ça, car il n'y a pas de taille de packet)
	sprintf(buffer, "x: %f Y: %f Z:%f\r\n", displayValueX, displayValueY, displayValueZ);

	send(sock , buffer , strlen(buffer) , 0 ); 

    n = read( sock , buffer, 1024); 
	//printf("\e[6;1H");
    printf("%s\n",buffer ); 

}

void* tcpSendThread(void* arg){
	struct sockaddr_in servaddr; 
	int sock;
	memset(&servaddr, 0, sizeof(servaddr)); 

	sock = socket(AF_INET, SOCK_STREAM, 0);

	// Infos client
	struct sockaddr_in localaddr = {0};
	localaddr.sin_family = AF_INET;
	localaddr.sin_addr.s_addr = inet_addr(TCP_CLIENT_ADDR);
	if(bind(sock, (struct sockaddr*)&localaddr, sizeof(struct sockaddr_in)) == 0) {
		printf("TCP bind success\n");
	} else {
		printf("TCP unable to bind\n");
	}


	// Infos server
	servaddr.sin_family = AF_INET; 
    servaddr.sin_port = htons(TCP_SERVER_PORT); 
	servaddr.sin_addr.s_addr = inet_addr(TCP_SERVER_ADDR);

	if(connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) ==0) {
		printf("TCP connect success\n");
	} else {
		printf("TCP unable to connect\n");
	}

	//printf("\e[6;1H");
	printf("Connected to TCP server\n");	

	while (1) {
		sendToTCPServer(sock);
		sleep(1);
	}
	close(sock);
} 

/*******************
*
* Ubidots
*
********************/

void sendToUbidotsServer(int sock){
	float displayValueX, displayValueY, displayValueZ;
	char ubidots_buffer[TCP_SERVER_BUFFER_SIZE], ubidots_response[TCP_SERVER_BUFFER_SIZE];
	int n;

	pthread_mutex_lock(&threadLock);
	displayValueX = values.X;
	displayValueY = values.Y;
	displayValueZ = values.Z;
	pthread_mutex_unlock(&threadLock);

	//En TCP il faut indiquer au serveur la fin de trame par \r\n (c'est le serveur qui demande ça, car il n'y a pas de taille de packet)
	sprintf(ubidots_buffer, "POST /api/v1.6/devices/SabreLite/AxeX/values HTTP/1.1\r\nHost: things.ubidots.com\r\nX-Auth-Token: BBFF-ze38IpunEPhqlTCWnJVbxDOCFFqqvg\r\nContent-Type: application/json\r\nUser-Agent: PostmanRuntime/7.19.0\r\nContent-Length: 18\r\n\r\n{\"value\": %1.4f}\r\n", displayValueX);

	send(sock , ubidots_buffer , strlen(ubidots_buffer) , MSG_CONFIRM ); 

    n = recv(sock, (char *)ubidots_response, TCP_SERVER_BUFFER_SIZE, 0);
	//printf("\e[6;1H");
    printf("%s\n", ubidots_response ); 

}

void* ubidotsSendThread(void* arg){
	
	char *ubidots_host = "things.ubidots.com";
	int sock;
	struct hostent *ubidots_server;
	struct sockaddr_in servaddr;

	memset(&servaddr, 0, sizeof(servaddr)); 

	sock = socket(AF_INET, SOCK_STREAM, 0);

	// Infos client
	struct sockaddr_in localaddr = {0};
	localaddr.sin_family = AF_INET;
	localaddr.sin_addr.s_addr = inet_addr(TCP_CLIENT_ADDR);
	if(bind(sock, (struct sockaddr*)&localaddr, sizeof(struct sockaddr_in)) == 0) {
		printf("TCP bind success\n");
	} else {
		printf("TCP unable to bind\n");
	}

	// Infos server
	// Lecture adresse IP du serveur Ubidots
	ubidots_server = gethostbyname(ubidots_host);
	if (ubidots_server == NULL) {
		printf("erreur recuperation adresse IP du serveur Ubidots");
		exit(EXIT_FAILURE);
	}

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(PORT_UBIDOTS);
	memcpy(&servaddr.sin_addr.s_addr, ubidots_server->h_addr, ubidots_server->h_length);

	if(connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) ==0) {
		printf("TCP connect success\n");
	} else {
		printf("TCP unable to connect\n");
	}

	//printf("\e[6;1H");
	printf("Connected to TCP server\n");

	while (1) {
		printf("sending to ubidots\n");
		sendToUbidotsServer(sock);
		sleep(15);
	}
	close(sock);
	

}


int main(void) {
	
	pthread_t capteurReadThread;
	pthread_t capteurDisplayThread;
	pthread_t capteurUDPDisplayThread;
	pthread_t capteurTCPDisplayThread;
	pthread_t capteurUbidotsDisplayThread;

	/* Il semble que l'initialisation du mutex de cette façon ne fonctionne pas
	if (pthread_mutex_init(&threadLock, NULL) != 0) { 
		printf("Impossible d'initialiser le mutex"); 
		return -11; 
	}*/

	printf("\e[2J");//Cleaning console

	if(pthread_create(&capteurReadThread, NULL, readThread, NULL) == -1) {
		printf("Impossible de creer le thread de lecture du capteur");
		return -1;
	}
	if(pthread_create(&capteurDisplayThread, NULL, displayThread, NULL) == -1) {
		printf("Impossible de creer le thread de d'affichage console");
		return -1;
	}
	if(pthread_create(&capteurUDPDisplayThread, NULL, updSendThread, NULL) == -1) {
		printf("Impossible de creer le thread de d'affichage udp");
		return -1;
	}
	if(pthread_create(&capteurTCPDisplayThread, NULL, tcpSendThread, NULL) == -1) {
		printf("Impossible de creer le thread de d'affichage tcp");
		return -1;
	}
	if(pthread_create(&capteurUbidotsDisplayThread, NULL, ubidotsSendThread, NULL) == -1) {
		printf("Impossible de creer le thread de d'affichage tcp");
		return -1;
	}
	while(1){sleep(10);}
	return 0;
}
