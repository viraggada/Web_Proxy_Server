/***************************************************************
   Author - Virag Gada
   File - proxyserver.c
   Description - Source file for proxy server
 ****************************************************************/

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdlib.h>
#include <strings.h>
#include <memory.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <openssl/md5.h>

#define MAX_BUFF_SIZE (1024)
#define BACKLOG (20)
#define MAX_CONNECTIONS (100)
#define DELIMITER (" ")
#define BLOCK_LIST ("blockedWebsites.txt")
#define HOSTNAME_CACHE ("hostnameIPCache")
/* Array of child threads and function descriptiors */
pthread_t child_threads[MAX_CONNECTIONS];
int client_fd[MAX_CONNECTIONS];

/* Variable for our socket fd*/
int sockfd;

/* Use for receive timeout */
struct timeval tv={0,0};

/* Global variable for timeout */
int timeout=0;

char errorTypes[7][50]={(int)'\0'};
int keepAliveTime;
int doPersistent = 0;
sig_atomic_t quit_request = 0;
uint16_t active_count = 0;
FILE * nameCache;

char * calculateMD5(const char * buffer,long int fileSize);
int checkCache(char * file, int socket);
void *respondClient(void * num);
int checkBlocked(char * website);
void storeHostnameIPCache(char * hostname, char * ip);
void checkHostnameIPCache(char * hostname, char * ip_addr);
int checkAndCreateDirectory(char * path);

int main(int argc, char *argv[])
{
  /*Variables*/
  long availableSlot = 0;
  struct sockaddr_in *selfAddr;
  struct sockaddr_in fromAddr;
  socklen_t fromAddrSize;
  char buffer[MAX_BUFF_SIZE];

  /* Store the error types */
  strcpy(errorTypes[0],"400 Bad Request Reason: Invalid Method");
  strcpy(errorTypes[1],"400 Bad Request Reason: Invalid URL");
  strcpy(errorTypes[2],"400 Bad Request Reason: Invalid HTTP-Version");
  strcpy(errorTypes[3],"403 Forbidden");
  strcpy(errorTypes[4],"404 Not Found Reason: URL does not exist");
  strcpy(errorTypes[5],"501 Not Implemented");
  strcpy(errorTypes[6],"500 Internal Server Error");

  pthread_attr_t attr;
  int status = checkAndCreateDirectory("cache");

  if(status == 0)
    exit(1);

  if(argc != 3){
    printf("%s\n","Usage ./webproxy <port> <timeout>");
    exit(1);
  }

  keepAliveTime = atoi(argv[2]);

  selfAddr = (struct sockaddr_in *)calloc(1,sizeof(struct sockaddr_in));

  (*selfAddr).sin_family = AF_INET;           //address family
  (*selfAddr).sin_port = htons(atoi(argv[1])); //sets port to network byte order
  (*selfAddr).sin_addr.s_addr = htonl(INADDR_ANY); //sets local address

  /*Create Socket*/
  if((sockfd = socket((*selfAddr).sin_family,SOCK_STREAM,0))< 0) {
    printf("Unable to create socket\n");
    exit(1);
  }
  //printf("Socket Created\n");

  /*Call Bind*/
  if(bind(sockfd,(struct sockaddr *)selfAddr,sizeof(*selfAddr))<0) {
    printf("Unable to bind\n");
    exit(1);
  }
  //printf("Socket Binded\n");

  /* Listen for incomming connections */
  if(listen(sockfd,BACKLOG)!=0){
    printf("%s\n","Listen Error");
    exit(1);
  }

  /* Add a signal handler for our SIGINT signal ( Ctrl + C )*/
  //signal(SIGINT,closingHandler);

  memset(&fromAddr,0,sizeof(fromAddr));
  memset(buffer,0,sizeof(buffer));
  fromAddrSize = sizeof(fromAddr);
  memset(client_fd,-1,sizeof(client_fd));
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  /*listen*/
  while(1) {
    //printf("waiting for connections..\n");
    /*Accept*/
    if((client_fd[availableSlot] = accept(sockfd,(struct sockaddr *)&fromAddr,&fromAddrSize)) < 0)
    {
      printf("Failed to accept connection\n");
      break;
    }else{
      /* Create new thread to handle the client */
      pthread_create(&child_threads[availableSlot],&attr,respondClient,(void *)availableSlot);
    }
    /* Add client fd to a position in the array which is empty */
    while (client_fd[availableSlot]!=-1) {
      availableSlot = (availableSlot+1)%MAX_CONNECTIONS;
    }
    sleep(0.2);
    //printf("Slot number - %ld\n", availableSlot);
  }

  /*Close*/
  close(sockfd);
  return 0;
}


/* Thread function to respond to client request */
void *respondClient(void * num){

  int client_no = (int )(long) num;
  //printf("Num - %d\n",client_no);
  FILE *fptr;
  char dataBuffer[MAX_BUFF_SIZE]={(int)'\0'};
  int size_to_send;
  char clientCommand[1024] = {(int)'\0'};
  struct stat file_info;
  char filePath[50] = {(int)'\0'};
  struct hostent *hostToconnect;
  struct sockaddr_in actualServerAddr;
  char * fileMD5 = NULL;
  char stringMD5[MAX_BUFF_SIZE]= {(int)'\0'};
  char fName[MAX_BUFF_SIZE] = {(int)'\0'};
  char fNameAndPath[MAX_BUFF_SIZE] = {(int)'\0'};
  int hostfd;
  int on = 1;
  int byteCount;
  int i;

  /* Structures for HTTP request and response */
  struct {
    char requestType[10];
    char path[MAX_BUFF_SIZE];
    char website[MAX_BUFF_SIZE];
    char url[MAX_BUFF_SIZE];
    char httpType[9];
    char connectionPersistance[10];
  }httpRequest;

  struct {
    char httpType[9];
    char contentTypeToSend[20];
    char fileRequired[50];
    int sizeOfFileBeingSent;
    char connectionPersistance[10];
  }httpResponse;

  /* String to hold the response header and error string */
  char responseHeader[200] = {(int)'\0'};
  char errorString[100] = {(int)'\0'};

  int val,activity;
  char * location;
  char postData[100];

  //set of socket descriptors
  fd_set readfds;

  tv.tv_sec = keepAliveTime;
  active_count++;
  printf("\nChild Number : %d\n", client_no);
  memset(&httpResponse,(int)'\0',sizeof(httpResponse));
  memset(&httpRequest,(int)'\0',sizeof(httpRequest));
  // If any action on given file descriptor then proceed
  if((val = recv(client_fd[client_no],clientCommand,2048,0))<=0){
      //printf("Value is %d\n", val);
      /* Exit on error */
      //printf("Exiting Child Number : %d\n", client_no);
      doPersistent = 0;
      perror("Error on recv:");
    }else if(strlen(clientCommand) == 0){
      /* Handling NULL data from client */
      printf("%s\n","NULL data");
    }else {
      //printf("\n%s\n","Received from client:");
      //printf("%s\n",clientCommand);

      memset(&httpRequest,(int)'\0',sizeof(httpRequest));
      /* Split the client request to handle */
      strcpy(httpRequest.requestType,strtok(clientCommand,DELIMITER));
      strcpy(httpRequest.path,strtok(NULL,DELIMITER));
      strncpy(httpRequest.httpType,strtok(NULL,DELIMITER),8);
      printf("%s %s %s %s\n",httpRequest.requestType,httpRequest.path,httpRequest.httpType,httpRequest.connectionPersistance);

      /* Handle GET method */
      if(strcmp(httpRequest.requestType,"GET")==0){

        /* Check if HTTP version is correct else send error*/
        if((strncmp(httpRequest.httpType,"HTTP/1.0",8)!=0)&&(strncmp(httpRequest.httpType,"HTTP/1.1",8)!=0)){
          sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n","HTTP/1.1", "400 Bad Request : Invalid HTTP Version");
          write(client_fd[client_no],responseHeader,strlen(responseHeader));
          memset(responseHeader,(int)'\0',sizeof(responseHeader));
          sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>\r\n\r\n",errorTypes[2],httpRequest.httpType);
          write(client_fd[client_no],errorString, strlen(errorString));
          memset(errorString,(int)'\0',sizeof(errorString));
        }else{
          /* Find the web address */
          location = strstr(httpRequest.path,"//");
          location += 2;

          for(i=0;i<strlen(location);i++)
          {
            if(location[i]=='/')
              break;
            httpRequest.website[i]=location[i];
          }
          /* Go till the beginning of path and get the rest of the url */
          location = strstr(location,"/");
          strcpy(httpRequest.url,location);

          printf("Website - %s, URL - %s\n",httpRequest.website,httpRequest.url);
          /* Check if the website is blocked */
          int check = checkBlocked(httpRequest.website);

          if(check == 0){
            sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "ERROR 403 Forbidden");
            write(client_fd[client_no],responseHeader,strlen(responseHeader));
            memset(responseHeader,(int)'\0',sizeof(responseHeader));
            sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>",errorTypes[3],"Blocked website");
            write(client_fd[client_no],errorString, strlen(errorString));
            memset(errorString,(int)'\0',sizeof(errorString));
          }else{
            /* Get the MD5sum as we store the url based on that in our cache */
            fileMD5 = calculateMD5(httpRequest.path,strlen(httpRequest.path));

            // computes the md5sum of the file
            for (i = 0; i< MD5_DIGEST_LENGTH; i++)
            {
              sprintf(&stringMD5[2*i],"%02x", fileMD5[i]);
            }
            free(fileMD5);
            sprintf(fName,"%s.html",stringMD5);
            //printf("File name %s\n", fName);
            sprintf(fNameAndPath,"%s/%s","cache", fName);
            //printf("Path with filename - %s\n", fNameAndPath);

            /* Check if the file is url is already present in our cache */
            if(checkCache(fNameAndPath,client_no)==1){
              //printf("%s\n","Found in Cache");
            }
            else{/* If not then create a new file of that name */
              if((fptr = fopen(fNameAndPath,"wb+"))==NULL){
                printf("unable to create file\n");
                sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "500 Internal Server Error");
                write(client_fd[client_no],responseHeader,strlen(responseHeader));
                memset(responseHeader,(int)'\0',sizeof(responseHeader));
                sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>",errorTypes[5],"File creation error");
                write(client_fd[client_no],errorString, strlen(errorString));
                memset(errorString,(int)'\0',sizeof(errorString));
              }else{
                char ip_addr[20];
                printf("%s\n","Checking Hostname-IP cache");
                /* Check if an IP address was passed in */
                if(((int)httpRequest.website[0]>(int)'0')&&((int)httpRequest.website[0]<(int)'9')){
                  strcpy(ip_addr,httpRequest.website);
                }else{ /* If not then check if we have already resolved the IP from hostname */
                checkHostnameIPCache(httpRequest.website,ip_addr);
                }
                if(ip_addr[0] == (int)'\0'){/* If we haven't resolved then get the IP from DNS */
                  printf("%s\n","Fetching from DNS");
                  hostToconnect = gethostbyname(httpRequest.website);
                  storeHostnameIPCache(httpRequest.website,inet_ntoa(*(struct in_addr*)(hostToconnect->h_addr)));
                }
                if ((!hostToconnect) && (!ip_addr)){
                    perror("Inavlid host address");
                    //exit(1);
                }else{
                  // parameters to connect to host server
                  bzero(&actualServerAddr,sizeof(actualServerAddr));       //zero the struct
                  actualServerAddr.sin_family = AF_INET;                   //address family
                  actualServerAddr.sin_port = htons(80);                   //htons() sets the port # to network byte order
                  if(ip_addr[0] == (int)'\0'){
                    memcpy(&actualServerAddr.sin_addr, hostToconnect->h_addr, hostToconnect->h_length);
                  }else{
                    actualServerAddr.sin_addr.s_addr = inet_addr(ip_addr);
                  }
                  int len = sizeof(actualServerAddr);
                  hostfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

                  if (hostfd<0)
                  {
                      perror("Host socket creation failed");
                  }

                  int serverSocket = connect(hostfd, (struct sockaddr *) &actualServerAddr, len);
                  if (serverSocket < 0)
                  {
                    printf("Error connecting to server\n");
                    close(hostfd);
                  }else{
                    //printf("Connected to server IP- %s\n",inet_ntoa(actualServerAddr.sin_addr));
                    if(httpRequest.url != NULL){
                      sprintf(responseHeader,"GET %s %s\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",httpRequest.url,httpRequest.httpType,httpRequest.website);
                    }else{
                      sprintf(responseHeader,"GET / %s\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n",httpRequest.httpType,httpRequest.website);
                    }
                    //printf("request to remote host \n%s", responseHeader);

                    byteCount = write(hostfd, responseHeader, sizeof(responseHeader));
                    //printf("Request size - %d\n",byteCount);
                    if(byteCount<0){
                      perror("Error sending to remote server");
                    }else{
                      //printf("%s\n","Receiving file from the server");
                      do{
                        memset(dataBuffer,(int)'\0',MAX_BUFF_SIZE);

                        byteCount = recv(hostfd, dataBuffer, MAX_BUFF_SIZE,0);
                        //printf("Received - %d bytes from server\n",byteCount);
                        if(byteCount>0){/* Receive file, sent to the web browser and store in local cache */
                          write(client_fd[client_no],dataBuffer,byteCount);
                          fwrite(dataBuffer,1,byteCount,fptr);
                        }
                      }while(byteCount>0);
                      printf("Sent - %s\n", httpRequest.path);
                    }
                    close(hostfd);
                  }
                }

                fclose(fptr);
              }
            }
          }
        }
      }/* If method not supported send 400 Error */
      else{
        printf("Method not supported - %s\n", httpRequest.requestType);
        sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "400 Bad Request");
        write(client_fd[client_no],responseHeader,strlen(responseHeader));
        memset(responseHeader,(int)'\0',sizeof(responseHeader));
        sprintf(errorString,"<html><body>%s: %s</body></html>",errorTypes[0],httpRequest.requestType);
        write(client_fd[client_no],errorString, strlen(errorString));
        memset(errorString,(int)'\0',sizeof(errorString));
      }
    memset(clientCommand,(int)'\0',strlen(clientCommand));
  }

  memset(stringMD5, (int)'\0', sizeof(stringMD5));
  memset(fName,(int)'\0', sizeof(fName));
  active_count--;
  printf("%s child %d\n","Disconnected",client_no);
  /* Close the thread socket */
  close(client_fd[client_no]);

  /* Set its value to -1 to use this position for some other thread */
  client_fd[client_no]=-1;
  sleep(keepAliveTime);
  remove(fNameAndPath);
  memset(fNameAndPath,(int)'\0', sizeof(fNameAndPath));
  pthread_exit(NULL);
}

/* Function to check cache for requested url */
int checkCache(char * file,int num){
  FILE *fptr;
  char dataBuffer[MAX_BUFF_SIZE]={(int)'\0'};
  struct stat file_info;
  int size_to_send;
  /* String to hold the response header and error string */
  char responseHeader[100] = {(int)'\0'};
  char errorString[100] = {(int)'\0'};

  if((fptr = fopen(file,"r")) == NULL){
    printf("%s\n","Not present in cache");
    return 0;
  }else{
    printf("%s\n","Present in cache");
    /* Get the file information */
    fstat(fileno(fptr),&file_info);
    /* Read the file size */
    long int file_size=file_info.st_size;
    //printf("file size ........... %ld\n",file_size);
    size_to_send = MAX_BUFF_SIZE;

    while(file_size>=0)
    {
      if(file_size<MAX_BUFF_SIZE)
      {
        size_to_send = file_size;
      }
      memset(dataBuffer,(int)'\0',sizeof(dataBuffer));
      if(fread(dataBuffer,size_to_send,1,fptr)<=0)
      { /* If error in reading file send 501 error */
        printf("unable to copy file into buffer\n");
        sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n","HTTP/1.1", "500 Internal Server Error");
        write(client_fd[num],responseHeader,strlen(responseHeader));
        memset(responseHeader,(int)'\0',sizeof(responseHeader));
        sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>",errorTypes[5],"File reading error");
        write(client_fd[num],errorString, strlen(errorString));
        memset(errorString,(int)'\0',sizeof(errorString));
      }else if(write(client_fd[num],dataBuffer,size_to_send)<=0) {
        printf("error in sending the file\n");
      }
      file_size-=MAX_BUFF_SIZE;
    }
    //printf("%s\n","File sent");
    fclose(fptr);
    return 1;
  }
}

/* Function to check if website is in blocked list */
int checkBlocked(char * website){
  FILE * fptr;
  char dataBuffer[MAX_BUFF_SIZE]={(int)'\0'};
  if((fptr=fopen(BLOCK_LIST,"r"))==NULL){
    printf("%s\n","Error opening file");
    return 0;
  }else{
    if(fread(dataBuffer,1,MAX_BUFF_SIZE,fptr)<=0){
      printf("%s\n","Error reading from file");
      fclose(fptr);
      return 0;
    }else{
      if(strstr(dataBuffer,website)==NULL){
        fclose(fptr);
        return 1;
      }else{
        printf("%s\n","Blocked website found");
        fclose(fptr);
        return 0;
      }
    }
  }
}

/* Function to store Hostname IP pair */
void storeHostnameIPCache(char * hostname, char * ip){
  char pair[MAX_BUFF_SIZE] = {(int)'\0'};
  FILE *fp;
  sprintf(pair,"%s:%s\n",hostname,ip);
  fp = fopen(HOSTNAME_CACHE,"a");
  fwrite(pair, 1, strlen(pair),fp);
  fclose(fp);
  return;
}

/* Function to check if Hostname is present in cache */
void checkHostnameIPCache(char * hostname,char * ip_addr){
  FILE *fp;
  char buff[MAX_BUFF_SIZE];
  char * pos;
  //printf("%s\n","Reading file");
  fp = fopen(HOSTNAME_CACHE,"r");
  fread(buff,1,MAX_BUFF_SIZE,fp);
  fclose(fp);
  //printf("%s\n","Searching buffer");
  pos = strstr(buff,hostname);
  if(pos != NULL){
    pos = pos + strlen(hostname)+1;
    if((sscanf(pos,"%s",ip_addr))==1){
      printf("IP from cache - %s\n",ip_addr);
      return;
    }
  }
  ip_addr[0] = (int)'\0';
  return;
}

/* Function to calculate MD5 sum of a buffer */
char * calculateMD5(const char * buffer,long int fileSize){
  char *md5_res = NULL;
  md5_res = (uint8_t *)calloc(1,MD5_DIGEST_LENGTH*2+1);
  unsigned char digest[16];
  MD5_CTX c;
  int n;

  MD5_Init(&c);

  while(fileSize>0){
    if(fileSize>512){
      MD5_Update(&c, buffer, 512);
    }else{
      MD5_Update(&c, buffer, fileSize);
    }
    fileSize -= 512;
    buffer += 512;
  }
  MD5_Final(digest, &c);
  for (n = 0; n < 16; ++n) {
      snprintf(&(md5_res[n*2]), 16*2, "%02x", (unsigned int)digest[n]);
  }
  return md5_res;
}

/* Function to create directory */
int checkAndCreateDirectory(char * path){
  struct stat sd;
  int ret = stat(path,&sd);

  if(ret == 0){
    if(sd.st_mode & S_IFDIR){
      printf("%s directory is present\n",path);
    }
    return 1;
  }else{
    if(errno == ENOENT){
      printf("%s\n","Directory does not exist. Creating new directory...");
      ret = mkdir(path,S_IRWXU);
      if(ret != 0){
        printf("%s\n","mkdir failed");
        perror("Failed due to ");
      }
      else {
        printf("Created directory - %s\n",path);
      }
      return 1;
    }
  }
  return 0;
}
