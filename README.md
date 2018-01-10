# ECEN 5273: Network Systems: Programming Assignment 4 #

Goal: To create a webproxy that handles multiple simultaneous requests from multiple clients,
      pass them to the HTTP server and handle returning traffic from HTTP server to clients.

File structure:
```
./README.txt
./proxyserver.c
./Makefile
./hostnameIPCache
./blockedWebsites.txt
./cache/
```

proxyserver.c: The program accepts HTTP requests from multiple clients and responds
          to them, passes them to the HTTP server and handle returning traffic from
          HTTP server to clients.

hostnameIPCache: Cache storing the hostname to IP conversions from DNS.

blockedWebsites.txt: File containing the websites to block.

Methods Supported: GET

HTTP Version Support: HTTP/1.0 and HTTP/1.1

Pipelining Support: No

Errors Handled:
```
400 Bad request
500 Internal Server Error
501 Not Implemented
```

Testing using web browser: http://localhost:<Port number>/index.html

Testing using Telnet:
```
telnet <ip> <port>
GET </path> HTTP/1.1
```
Execution:
```
Proxy side - Go to the folder.
             Run make.
             Run the server using ./proxyserver <port> <timeout>
```
