/***************************************************************
   Author - Virag Gada
   File - server.c
   Description - Source file for web server
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

#define MAX_BUFF_SIZE (1024)
#define BACKLOG (20)
#define MAX_CONNECTIONS (100)
#define CONFIG_FILE ("ws.conf")
#define DELIMITER (" ")

/* Array's to store a variety of information */
char listenPort[5];
char fileSystemLocation[50]={(int)'\0'};
char defaultFiles[3][20]={(int)'\0'};
char contentExtension[50][10]={(int)'\0'};
char contentType[50][20]={(int)'\0'};
char errorTypes[6][50]={(int)'\0'};
int keepAliveTime = 0;
int doPersistent = 0;

sig_atomic_t quit_request = 0;
uint16_t active_count = 0;
/* Use for receive timeout */
struct timeval tv={0,0};

/* Array of child threads and function descriptiors */
pthread_t child_threads[MAX_CONNECTIONS];
int client_fd[MAX_CONNECTIONS];

/* Variable for our socket fd*/
int sockfd;

/* Function prototypes */
int readConfig(void);
void *respondClient(void * num);
void closingHandler(int mySignal);

//int delete(char * file);
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
  strcpy(errorTypes[3],"404 Not Found Reason: URL does not exist");
  strcpy(errorTypes[4],"501 Not Implemented");
  strcpy(errorTypes[5],"500 Internal Server Error");

  pthread_attr_t attr;
  int status = readConfig();

  if(status == 0)
    exit(1);

  selfAddr = (struct sockaddr_in *)calloc(1,sizeof(struct sockaddr_in));

  (*selfAddr).sin_family = AF_INET;           //address family
  (*selfAddr).sin_port = htons(atoi(listenPort)); //sets port to network byte order
  (*selfAddr).sin_addr.s_addr = htonl(INADDR_ANY); //sets local address

  /*Create Socket*/
  if((sockfd = socket((*selfAddr).sin_family,SOCK_STREAM,0))< 0) {
    printf("Unable to create socket\n");
    exit(1);
  }
  printf("Socket Created\n");

  /*Call Bind*/
  if(bind(sockfd,(struct sockaddr *)selfAddr,sizeof(*selfAddr))<0) {
    printf("Unable to bind\n");
    exit(1);
  }
  printf("Socket Binded\n");

  /* Listen for incomming connections */
  if(listen(sockfd,BACKLOG)!=0){
    printf("%s\n","Listen Error");
    exit(1);
  }

  /* Add a signal handler for our SIGINT signal ( Ctrl + C )*/
  signal(SIGINT,closingHandler);

  memset(&fromAddr,0,sizeof(fromAddr));
  memset(buffer,0,sizeof(buffer));
  fromAddrSize = sizeof(fromAddr);
  memset(client_fd,-1,sizeof(client_fd));
  pthread_attr_init(&attr);
  //pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  /*listen*/
  while(1) {
    printf("waiting for connections..\n");
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
    printf("Slot number - %ld\n", availableSlot);
  }

  /*Close*/
  close(sockfd);
  return 0;
}

/* Handler function for SIGINT (Ctrl+C) signal*/
void closingHandler(int mySignal){
  /* Set the global quit flag */
  quit_request = 1;
  int i = 0;

  /* Wait for all active threads to close */
  while(active_count>0){
    for(i = 0; i<MAX_CONNECTIONS ; i++){
      /* If thread is running wait to exit and join */
      if(client_fd[i]!=-1){
        pthread_join(child_threads[i],NULL);
      }
    }
  }
  /* Close the server sock fd and exit */
  close(sockfd);
  exit(0);
}

/* Thread function to respond to client request */
void *respondClient(void * num){

  int client_no = (int )(long) num;
  //printf("Num - %d\n",client_no);
  FILE *fptr;
  char dataBuffer[MAX_BUFF_SIZE]={(int)'\0'};
  int size_to_send;
  char clientCommand[2048] = {(int)'\0'};
  struct stat file_info;
  char filePath[50] = {(int)'\0'};
  int i;

  /* Structures for HTTP request and response */
  struct {
    char requestType[10];
    char file[50];
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
  char responseHeader[100] = {(int)'\0'};
  char errorString[100] = {(int)'\0'};

  int val,activity;
  char * location;
  char postData[100];

  //set of socket descriptors
  fd_set readfds;

  tv.tv_sec = keepAliveTime;
  active_count++;
  do{
    /* Empty the set */
    FD_ZERO(&readfds);
    /* Add our client fd to this set */
    FD_SET(client_fd[client_no], &readfds);

    printf("%s\n","Waiting for new request");
    /* Monitor the file descriptor */
    activity = select(client_fd[client_no] + 1 , &readfds , NULL , NULL , &tv);
    if (activity < 0)
    {
      //printf("Select Error - Activity - %d\n", activity);
      printf("Error on select %s\n", strerror(errno));
      break;
    }else{

      printf("Child Number : %d\n", client_no);
      memset(&httpResponse,(int)'\0',sizeof(httpResponse));
      memset(&httpRequest,(int)'\0',sizeof(httpRequest));
      /* If any action on given file descriptor then proceed */
      if (FD_ISSET(client_fd[client_no] , &readfds)) {
        if((val = recv(client_fd[client_no],clientCommand,2048,MSG_DONTWAIT))<=0){
          //printf("Value is %d\n", val);
          /* Exit on error */
          printf("Exiting Child Number : %d\n", client_no);
          doPersistent = 0;
          //printf("Error on recv %s\n", strerror(errno));
        }else if(strlen(clientCommand) == 0){
          /* Handling NULL data from client */
          printf("%s\n","NULL data");
        }else {
          printf("\n%s\n","Received from client:");
          //printf("%s\n",clientCommand);

          /* Enable pipelining if requested by client */
          if(strstr(clientCommand,"keep-alive")!=NULL){
            doPersistent = 1;
            tv.tv_sec = keepAliveTime;
            strcpy(httpRequest.connectionPersistance,"keep-alive");
          }else{
            /* Else disable pipelining */
            doPersistent = 0;
            tv.tv_sec = 0;
            //setsockopt(client_fd[client_no], SOL_SOCKET, SO_RCVTIMEO,(struct timeval *)&tv,sizeof(struct timeval));
            strcpy(httpRequest.connectionPersistance,"close");
          }

          /* Check for POST DATA for POST method */
          if(strstr(clientCommand,"POST")!=NULL){
            if((location = strstr(clientCommand,"\r\n\r\n"))!=NULL){
              location = location + strlen("\r\n\r\n");
              strcpy(postData,location);
            }
            printf("Post Data - %s\n", postData);
          }
          /* Split the client request to handle */
          strcpy(httpRequest.requestType,strtok(clientCommand,DELIMITER));
          strcpy(httpRequest.file,strtok(NULL,DELIMITER));
          strncpy(httpRequest.httpType,strtok(NULL,DELIMITER),8);
          printf("%s %s %s %s\n",httpRequest.requestType,httpRequest.file,httpRequest.httpType,httpRequest.connectionPersistance);

          /* Handle GET method */
          if(strcmp(httpRequest.requestType,"GET")==0){
            /* Check for the content type of the file */
            for(i=0; i<9; i++){
              if((strstr(httpRequest.file,contentExtension[i]))!=NULL){
                strcpy(httpResponse.contentTypeToSend,contentType[i]);
                printf("Content type to send - %s\n", httpResponse.contentTypeToSend);
                break;
              }else{
                /* If '/' then it is index.html so take text/html as content type */
                if(strcmp(httpRequest.file,"/")==0){
                  strcpy(httpResponse.contentTypeToSend,"text/html");
                }else{
                  /* Else content type not supported */
                  strcpy(httpResponse.contentTypeToSend,"0");
                }
              }
            }
            /* Send 400 Bad request error for invalid content type */
            if(strncmp(httpResponse.contentTypeToSend,"0",1)==0){
              printf("Content Type not supported - %s\n", httpRequest.file);
              sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "400 Bad Request");
              write(client_fd[client_no],responseHeader,strlen(responseHeader));
              memset(responseHeader,(int)'\0',sizeof(responseHeader));
              sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>\r\n\r\n",errorTypes[1],httpRequest.file);
              write(client_fd[client_no],errorString, strlen(errorString));
              memset(errorString,(int)'\0',sizeof(errorString));
            }else{
              /* Check if HTTP version is correct else send error*/
              if((strncmp(httpRequest.httpType,"HTTP/1.0",8)!=0)&&(strncmp(httpRequest.httpType,"HTTP/1.1",8)!=0)){
                sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n","HTTP/1.1", "400 Bad Request");
                write(client_fd[client_no],responseHeader,strlen(responseHeader));
                memset(responseHeader,(int)'\0',sizeof(responseHeader));
                sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>\r\n\r\n",errorTypes[2],httpRequest.httpType);
                write(client_fd[client_no],errorString, strlen(errorString));
                memset(errorString,(int)'\0',sizeof(errorString));
              }else{
                /* Check if it is any of the default files */
                for(i=0;i<4;i++){
                  if(strcmp(defaultFiles[i],httpRequest.file)==0){
                    strcpy(httpRequest.file,"/index.html");
                  }
                }

                /* Find file based on reference from the location in config file */
                strcpy(filePath,fileSystemLocation);
                strcpy(&filePath[strlen(fileSystemLocation)],httpRequest.file);
                //printf("File required by server - %s\n",filePath);
                //printf("Content type required - %s\n", httpResponse.contentTypeToSend);
                if((fptr=fopen(filePath,"r")) == NULL)
                { /* If file not found then send 404 Not found error */
                  sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "404 Not Found");
                  write(client_fd[client_no],responseHeader,strlen(responseHeader));
                  printf("%s\n",responseHeader);
                  memset(responseHeader,(int)'\0',sizeof(responseHeader));
                  sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>\r\n\r\n",errorTypes[3],httpRequest.file);
                  write(client_fd[client_no],errorString, strlen(errorString));
                  memset(errorString,(int)'\0',sizeof(errorString));;
                  printf("%s\n","File not found");
                }
                else{
                  /* Get the file information */
                  fstat(fileno(fptr),&file_info);
                  /* Read the file size */
                  long int file_size=file_info.st_size;
                  //printf("file size ........... %ld\n",file_size);
                  size_to_send = MAX_BUFF_SIZE;

                  /* Create the response structure */
                  httpResponse.sizeOfFileBeingSent = file_size;
                  strncpy(httpResponse.httpType,httpRequest.httpType,8);
                  strncpy(httpResponse.connectionPersistance,httpRequest.connectionPersistance,10);
                  /* Create header for the file to send */
                  sprintf(responseHeader,"%s 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n",
                    httpResponse.httpType,httpResponse.contentTypeToSend,httpResponse.sizeOfFileBeingSent,httpResponse.connectionPersistance);
                  //printf("Response header - \n%s\n", responseHeader);
                  send(client_fd[client_no], responseHeader ,strlen(responseHeader),0);

                  //printf("%s\n","Sending File..........");
                  /* Break file into pieces and send */
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
                      sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "500 Internal Server Error");
                      write(client_fd[client_no],responseHeader,strlen(responseHeader));
                      memset(responseHeader,(int)'\0',sizeof(responseHeader));
                      sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>",errorTypes[5],"File reading error");
                      write(client_fd[client_no],errorString, strlen(errorString));
                      memset(errorString,(int)'\0',sizeof(errorString));
                    }else if(write(client_fd[client_no],dataBuffer,size_to_send)<=0) {
                      printf("error in sending the file\n");
                    }
                    file_size-=MAX_BUFF_SIZE;
                  }
                  //printf("%s\n","File sent");
                  fclose(fptr);
                }
              }
            }
          }/* Handle POST method */
          else if(strcmp(httpRequest.requestType,"POST")==0){
            /* Check for the content type of the file */
            for(i=0; i<9; i++){
              if((strstr(httpRequest.file,contentExtension[i]))!=NULL){
                strcpy(httpResponse.contentTypeToSend,contentType[i]);
                printf("Content type to send - %s\n", httpResponse.contentTypeToSend);
                break;
              }else{
                /* If '/' then it is index.html so take text/html as content type */
                if(strcmp(httpRequest.file,"/")==0){
                  strcpy(httpResponse.contentTypeToSend,"text/html");
                }else{
                  /* Else content type not supported */
                  strcpy(httpResponse.contentTypeToSend,"0");
                }
              }
            }
            /* Send 400 Bad request error for invalid content type */
            if(strncmp(httpResponse.contentTypeToSend,"0",1)==0){
              printf("Content Type not supported - %s\n", httpRequest.file);
              sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "400 Bad Request");
              write(client_fd[client_no],responseHeader,strlen(responseHeader));
              memset(responseHeader,(int)'\0',sizeof(responseHeader));
              sprintf(errorString,"<html><body>%s: %s</body></html>",errorTypes[1],httpRequest.file);
              write(client_fd[client_no],errorString, strlen(errorString));
              memset(errorString,(int)'\0',sizeof(errorString));
            }else{
                /* Check if HTTP version is correct else send error*/
              if((strncmp(httpRequest.httpType,"HTTP/1.0",8)!=0)&&(strncmp(httpRequest.httpType,"HTTP/1.1",8)!=0)){
                sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n","HTTP/1.1", "400 Bad Request");
                write(client_fd[client_no],responseHeader,strlen(responseHeader));
                memset(responseHeader,(int)'\0',sizeof(responseHeader));
                sprintf(errorString,"<html><body>%s: <<%s>></body></html>",errorTypes[2],httpRequest.httpType);
                write(client_fd[client_no],errorString, strlen(errorString));
                memset(errorString,(int)'\0',sizeof(errorString));
              }else{
                /* Check if it is any of the default files */
                for(i=0;i<4;i++){
                  if(strcmp(defaultFiles[i],httpRequest.file)==0){
                    strcpy(httpRequest.file,"/index.html");
                  }
                }
                /* Find file based on reference from the location in config file */
                strcpy(filePath,fileSystemLocation);
                strcpy(&filePath[strlen(fileSystemLocation)],httpRequest.file);
                //printf("File required by server - %s\n",filePath);
                //printf("Content type required - %s\n", httpResponse.contentTypeToSend);
                if((fptr=fopen(filePath,"r")) == NULL)
                {  /* If file not found then send 404 Not found error */
                  sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "404 Not Found");
                  write(client_fd[client_no],responseHeader,strlen(responseHeader));
                  memset(responseHeader,(int)'\0',sizeof(responseHeader));
                  sprintf(errorString,"<html><body>%s: <<%s>></body></html>",errorTypes[3],httpRequest.file);
                  write(client_fd[client_no],errorString, strlen(errorString));
                  memset(errorString,(int)'\0',sizeof(errorString));
                  printf("%s\n","File not found");
                }
                else{
                  /* Get the file information */
                  fstat(fileno(fptr),&file_info);
                  /* Read the file size */
                  long int file_size=file_info.st_size;
                  //printf("file size ........... %ld\n",file_size);
                  size_to_send = MAX_BUFF_SIZE;
                  /* Create the response structure */
                  httpResponse.sizeOfFileBeingSent = file_size;
                  strncpy(httpResponse.httpType,httpRequest.httpType,8);
                  strncpy(httpResponse.connectionPersistance,httpRequest.connectionPersistance,10);
                  /* Create header for the file to send */
                  sprintf(responseHeader,"%s 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n",
                    httpResponse.httpType,httpResponse.contentTypeToSend,httpResponse.sizeOfFileBeingSent,httpResponse.connectionPersistance);
                  //printf("Response header - \n%s\n", responseHeader);
                  send(client_fd[client_no], responseHeader ,strlen(responseHeader),0);
                  //printf("%s\n","Sending File..........");
                  memset(dataBuffer,(int)'\0',sizeof(dataBuffer));

                  /* Prepend the post data */
                  sprintf(dataBuffer,"<html><body><pre><h1>%s</h1></pre>",postData);
                  if(write(client_fd[client_no],dataBuffer,sizeof(postData))<=0) {
                    printf("error in sending the file\n");
                  }
                  /* Break file into pieces and send */
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
                      sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "500 Internal Server Error");
                      write(client_fd[client_no],responseHeader,strlen(responseHeader));
                      memset(responseHeader,(int)'\0',sizeof(responseHeader));
                      sprintf(errorString,"<html><body><h1>%s: %s</h1></body></html>",errorTypes[5],"File reading error");
                      write(client_fd[client_no],errorString, strlen(errorString));
                      memset(errorString,(int)'\0',sizeof(errorString));
                    }else if(write(client_fd[client_no],dataBuffer,size_to_send)<=0) {
                      printf("error in sending the file\n");
                    }
                    file_size-=MAX_BUFF_SIZE;
                  }
                  //printf("%s\n","File sent");
                  fclose(fptr);
                }
              }
            }
          }/* If method not supported send 501 Error */
          else{
            printf("Method not supported - %s\n", httpRequest.requestType);
            sprintf(responseHeader,"%s %s\r\nContent-Type: text/html\r\n\r\n",httpRequest.httpType, "501 Not Implemented");
            write(client_fd[client_no],responseHeader,strlen(responseHeader));
            memset(responseHeader,(int)'\0',sizeof(responseHeader));
            sprintf(errorString,"<html><body>%s: %s</body></html>",errorTypes[4],httpRequest.requestType);
            write(client_fd[client_no],errorString, strlen(errorString));
            memset(errorString,(int)'\0',sizeof(errorString));
          }
        }
      }else{
        doPersistent = 0;
        printf("Error on isset %s\n", strerror(errno));
      }
      memset(clientCommand,(int)'\0',strlen(clientCommand));
    }
    /* Loop if pipelining is required */
  }while(doPersistent);
  active_count--;
  printf("%s\n","Disconnected");
  /* Close the thread socket */
  close(client_fd[client_no]);

  /* Set its value to -1 to use this position for some other thread */
  client_fd[client_no]=-1;
  pthread_exit(NULL);
}


/* Function to read the server configuration file */
int readConfig(void){
  FILE *cptr;
  char configBuffer[1024] = {0};
  /* Read the config file */
  if((cptr=fopen(CONFIG_FILE,"r"))==NULL)
  {
    return 0;
  }
  else{
    char *position;
    char *src;
    char temp[3][20];
    struct stat file_info;
    int i;
    /* Get the file information */
    fstat(fileno(cptr),&file_info);
    /* Read the file size */
    long int file_size=file_info.st_size;
    if(fread(configBuffer,file_size,1,cptr)<0){
      fclose(cptr);
      return 0;
    }
    src = strstr(configBuffer,"Listen");
    src = src+strlen("Listen")+1;
    //strncpy(listenPort,src,5);
    position = src;
    while(*position!='\n'){
      position++;
    }
    strncpy(listenPort,src,position-src);
    //if ((strlen(listenPort)>0) && (listenPort[strlen (listenPort) - 1] == '\n'))
    //  listenPort[strlen (listenPort) - 1] = '\0';
    printf("%s %s\n","The port number is - ",listenPort);
    /* Get the directory for server files */
    src = strstr(configBuffer,"DocumentRoot");
    src = src+strlen("DocumentRoot")+1;
    position = src;
    while(*position!='\n'){
      position++;
    }
    strncpy(fileSystemLocation,src,position-src);
    printf("%s %s\n","The root directory is - ",fileSystemLocation);

    src = strstr(configBuffer,"Content-Type");
    while(*src!='\n'){
      src++;
    }
    i=0;
    while(sscanf(src,"%s %s",contentExtension[i],contentType[i])==2){
      src = src + strlen(contentExtension[i])+strlen(contentType[i])+2;
      //printf("%s %s\n", contentExtension[i],contentType[i]);
      i++;
      if(i==9){
        break;
      }
    }
    /* Get the default files that point to index.html */
    src = strstr(configBuffer,"DirectoryIndex");
    src = src + strlen("DirectoryIndex");
    strcpy(defaultFiles[0],"/");
    sscanf(src,"%s %s %s",temp[0],temp[1],temp[2]);
    strcpy(defaultFiles[1],"/");
    strcat(defaultFiles[1],temp[0]);
    strcpy(defaultFiles[2],"/");
    strcat(defaultFiles[2],temp[1]);
    strcpy(defaultFiles[3],"/");
    strcat(defaultFiles[3],temp[2]);
    printf("Default files are - %s, %s, %s and %s\n",defaultFiles[0],defaultFiles[1],defaultFiles[2],defaultFiles[3]);

    /* Reada the keep alive time */
    src = strstr(configBuffer,"Keep-Alive");
    src = src + strlen("Keep-Alive")+strlen("time")+2;
    sscanf(src,"%d",&keepAliveTime);
    printf("Keep alive timeout %d\n",keepAliveTime);
  }
  fclose(cptr);
  return 1;
}
