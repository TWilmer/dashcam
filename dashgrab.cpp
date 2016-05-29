
#include <stdio.h>
 #include <strings.h>

#include <sys/types.h>          
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <wiringPi.h>

const int portno=3333;

#include <thread>
#include <future>
#include <sstream>

int run;

void error( char *msg ) {
  perror(  msg );
  exit(1);
}

void processClient(int fd, unsigned long address)
{
  printf("Processing client %x\n\r", address);
  std::ostringstream filename;
  filename <<"/var/www/html/grab";
  filename << address;
  filename << ".jpeg";
  int out=open(filename.str().c_str(), O_WRONLY | O_CREAT);
  fchmod(out, S_IROTH);
  char buffer[4*1024];
  int len;
  do
  {
    len=read(fd,buffer, sizeof(buffer));
    if(len>0)
      write(out,buffer, len);
  } while(len>0);

}

void acceptThread()
{
     int sockfd, newsockfd;
     sockfd = socket(AF_INET, SOCK_STREAM, 0);
	 if (sockfd < 0) 
         error( const_cast<char *>("ERROR opening socket") );

 struct sockaddr_in serv_addr,cli_addr;
 bzero((char *) &serv_addr, sizeof(serv_addr));

     serv_addr.sin_family = AF_INET;
     serv_addr.sin_addr.s_addr = INADDR_ANY;
     serv_addr.sin_port = htons( portno );
     if (bind(sockfd, (struct sockaddr *) &serv_addr,   sizeof(serv_addr)) < 0) 
       error( const_cast<char *>( "ERROR on binding" ) );
     listen(sockfd,5);
     while ( run ) {
        fd_set fds;
	struct timeval tv;
        printf( "waiting for new client...\n\r" );
        int clilen = sizeof(cli_addr);
        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        tv.tv_sec = 2; tv.tv_usec = 0;
	if(select(sockfd+1, &fds, NULL, NULL, &tv)>0)
	{
        if ( ( newsockfd = accept( sockfd, (struct sockaddr *) &cli_addr, (socklen_t*) &clilen) ) < 0 )
    		error( const_cast<char *>("ERROR on accept") );
         printf( "opened new communication with client\n\r" );
     std::async(std::launch::async,processClient, newsockfd, serv_addr.sin_addr.s_addr );
        }
	

   }
}
void sendCaptureGpio()
{
 pinMode (21, OUTPUT) ;
 digitalWrite (21, HIGH) ; delay (500) ;
 digitalWrite (21,  LOW) ; delay (500) ;
}
int main()
{
  run=1;
  wiringPiSetupGpio();

    std::thread t2(acceptThread); 
   char c;
 system ("/bin/stty raw");
   do {
     c= getchar();
     printf("You typed %c\n\r", c);
     switch(c)
     {
       case 'c': sendCaptureGpio(); break;
     }
 
   }while(c!='q');
  system ("/bin/stty cooked");
  run=0;
   t2.join();
}
