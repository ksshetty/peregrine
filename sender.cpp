#include <iostream>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mman.h>

#include "exceptions.h"
#include "settings.h"
#include "serializer.h"
#include "socketwrapper.h"
#include "packetformat.h"
#include "sender.h"

//#define LOG
using namespace std;

int Sender::sockIn;
int Sender::sockOut;
struct sockaddr_in *Sender::recvAddr;
int Sender::filefd;
void *Sender::mappedPtr;
char *Sender::bitmap;
PacketMetadata Sender::metadata;
int Sender::bitmapRemainder;

void PrintUsage(){
	cerr<<"USAGE: ./sender hostName fileName"<<endl;
}

int main(int argc, char *argv[]){
	if(argc != 3){
		cerr<<"Error: Invalid number of arguments"<<endl;
		PrintUsage();
		return -1;
	}
	try{
		char* hostName = argv[1];
		char* fileName = argv[2];
		struct addrinfo *hints, *res;
		hints = new addrinfo;
		hints->ai_family = AF_INET;
		SocketWrapper::GetAddrInfo(hostName, NULL, hints, &res);
		if(res == NULL || res->ai_family != AF_INET)
			throw EX_SOCK_ERRGETADDRINFO;
			
		
		Sender::Main((sockaddr_in*)res->ai_addr, fileName);
	}
	catch(int ex){
		switch(ex){
			case EX_SOCK_ERRGETADDRINFO:
				perror("getaddrinfo()");
				break;
			default:
				cerr<<"Exception occurred: "<<hex<<ex<<endl;
				break;
		}
		if(ex >= 0x0200 && ex < 0x0300){
			perror("exp:");
		}
	}
	return 0;
}

void Sender::Main(sockaddr_in *hostAddr, char* filePath){
	// UDP socket connection time
	hostAddr->sin_port = htons(RECVER_PORT);
	recvAddr = hostAddr;
	
	sockIn = SocketWrapper::Socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockOut = SocketWrapper::Socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	SocketWrapper::SetReuseSock(sockIn);
	SocketWrapper::SetReuseSock(sockOut);
	
	// Bind the sockIn to the UDP port
	struct sockaddr_in selfAddr;
	selfAddr.sin_family = AF_INET;
	selfAddr.sin_addr.s_addr = INADDR_ANY;
	selfAddr.sin_port = htons(SENDER_PORT);
	memset(selfAddr.sin_zero, 0, 8);
	SocketWrapper::Bind(sockIn, &selfAddr, sizeof(sockaddr_in));
	SocketWrapper::SetNonblock(sockIn);
	
	// Open the file
	filefd = open(filePath, O_RDONLY);
	if(filefd == -1)
		throw EX_FILE_NOOPEN;
	// Initialize the data
	struct stat fileStats;
	stat(filePath, &fileStats);
	
	unsigned int fileSize = fileStats.st_size;
	metadata.fileSize = fileSize;
	metadata.sizePacket = PACKET_SIZE;
	metadata.numPackets = fileSize / (PACKET_SIZE - sizeof(uint32_t));       // Phew, luckily caught this in time :)
	if(fileSize % PACKET_SIZE) metadata.numPackets++;
	
	// Map the file into virtual memory
	mappedPtr = mmap(0, fileSize, PROT_READ, MAP_SHARED, filefd, 0);
	if(mappedPtr == MAP_FAILED)
		throw EX_FILE_NOMAP;
	
	// Create the bitmap just before sending
	Sender::bitmap = new char[metadata.numPackets];
	memset(Sender::bitmap, 0, metadata.numPackets);
	bitmapRemainder = metadata.numPackets;
	//Create the bitmap linked list
	LinkedList *top = NULL, *bottom = NULL; 
	top = (LinkedList*)new LinkedList[metadata.numPackets];
	LinkedList *temp = top;
	for(unsigned int i=0;i<metadata.numPackets;i++){
		temp->num = i;
		temp->next = (temp + 1);
		temp->prev = (temp - 1);
		temp = temp->next;
	}
	bottom = top + (metadata.numPackets-1);
	bottom->next = top;
	top->prev = bottom;
	// First send the meta data 10 times (just to be sure)
	for(int j=0;j<10;j++)
		Sender::Sendmeta();
	cerr<<"Trying to send number of packets: "<<metadata.numPackets<<endl;
	// Now to flood the channel
	bool stopSending = false;
	LinkedList* current = top;
	while(!stopSending){
		try{
			unsigned int seqNum;
			while(Sender::RecvAck(seqNum)){
				// Received an ack.
				if(seqNum == metadata.numPackets){
					stopSending = true;
					cerr<<"INside if(seqNum >= metadata.numPackets)"<<endl;
					break;
				}
				if(seqNum > metadata.numPackets)
					continue;
				if(Sender::bitmap[seqNum] == 0){
					bitmapRemainder--;
				}
				Sender::bitmap[seqNum] = 1;
			}
		}
		catch(int ex){
			if(ex == EX_SOCK_ERRRECVFROM && errno == EWOULDBLOCK){
				// Send the "current" seq num
				if(Sender::bitmap[current->num] == 1){
					// Remove from linked list
					current->prev->next = current->next;
					current->next->prev = current->prev;
					current = current->next;
				}
				else{
#ifdef LOG
					cout<<current->num<<endl;
#endif
					if(Sender::SendChunk(current->num))
						current = current->next;
				}
				if(bitmapRemainder == 0)
					stopSending = true;
			}
		}
	}
	
	// Unmap the file and close it
	munmap(mappedPtr, metadata.fileSize);
	close(filefd);
}

bool Sender::RecvAck(unsigned int& seqNum){
	char buf[PACKET_SIZE + 50];
	struct sockaddr_in tempRecvAddr;
	socklen_t tempRecvAddrlen = sizeof(sockaddr_in);
	
	ssize_t recvLen = SocketWrapper::Recvfrom(sockIn, buf, sizeof(PacketAck), 0, &tempRecvAddr, &tempRecvAddrlen);
	if(recvLen == sizeof(PacketAck)){
		PacketAck ackPack;
		unserialize_Ack(buf, &ackPack);
		if(ackPack.type == TYPE_NACK){
			cerr<<"ReSending metadata"<<endl;
			Sender::Sendmeta();
		}		
		else{
			seqNum = ackPack.seqNum;
		}
	}
	return true;
}

void Sender::Sendmeta(){
	// Send the metadata
	char buf[sizeof(Packet_Metadata)];
	serialize_Metadata(&metadata, buf);
	Sender::Send(buf, sizeof(PacketMetadata));
	cerr<<"After sending metadata"<<endl;
}

bool Sender::Send(char* buf, ssize_t len){
	try{
		SocketWrapper::Sendto(sockOut, buf, len, 0, recvAddr, sizeof(sockaddr_in));
	}
	catch(int ex){
		if(ex == EX_SOCK_ERRSENDTO && errno == ENOBUFS)
			return false;
		else throw ex;
	}
	return true;
}

bool Sender::SendChunk(unsigned int packetNum){
	const ssize_t chunkSize = (PACKET_SIZE - sizeof(uint32_t));
	int readSize;
	if(packetNum == (metadata.numPackets - 1)){
		// Last packet
		readSize = (metadata.fileSize % (metadata.sizePacket-sizeof(uint32_t)))?(metadata.fileSize % (metadata.sizePacket-sizeof(uint32_t))):chunkSize;
	}
	else
		readSize = chunkSize;
	ssize_t packSize = readSize + sizeof(uint32_t);
	
	// Create a packet of max length
	char packBuf[chunkSize + sizeof(uint32_t)];
	PacketData *packet = (PacketData*)packBuf;
	packet->seqNum = packetNum;
	memcpy(packet->data, (char*)mappedPtr+(packetNum*chunkSize), readSize);
	
	char sendbuf[PACKET_SIZE];
	serialize_Data(packet, packSize, sendbuf);
	
	bool retVal = Sender::Send(sendbuf, packSize);

	return retVal;
}
