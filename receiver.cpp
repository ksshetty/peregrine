#include <iostream>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/mman.h>

#include "exceptions.h"
#include "settings.h"
#include "serializer.h"
#include "receiver.h"
#include "socketwrapper.h"

using namespace std;
int Receiver::sockIn;
int Receiver::sockOut;
struct sockaddr_in *Receiver::sendAddr;
PacketMetadata *Receiver::metadata;
char *Receiver::bitmap;
int Receiver::filefd;
void *Receiver::mappedPtr;
int Receiver::bitmapRemainder;

int main(int argc, char* argv[]){
	// No arguments, just start the receiver
	try{
		Receiver::Main();
	}
	catch(int ex){
		cerr<<"Exception: "<<hex<<ex<<endl;
		if(ex >= 0x0200 && ex < 0x0300){
			perror("exp:");
		}
	}
}

void Receiver::Main(){
	// UDP socket connection time
	sendAddr = new sockaddr_in;
	
	sockIn = SocketWrapper::Socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockOut = SocketWrapper::Socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	SocketWrapper::SetReuseSock(sockIn);
	SocketWrapper::SetReuseSock(sockOut);
	
	// Bind the sockIn to the UDP port
	struct sockaddr_in selfAddr;
	selfAddr.sin_family = AF_INET;
	selfAddr.sin_addr.s_addr = INADDR_ANY;
	selfAddr.sin_port = htons(RECVER_PORT);
	memset(selfAddr.sin_zero, 0, 8);
	SocketWrapper::Bind(sockIn, &selfAddr, sizeof(sockaddr_in));
	
	// Receive the metadata
	Receiver::ReceiveMeta();
	cerr<<"Received the metadata"<<endl;
	
	// The loop
	struct sockaddr_in senderAddr;
	socklen_t senderAddrLen = sizeof(sockaddr_in);
	
	char buf[PACKET_SIZE + 50];
	int count = 0;
	int skipCount = metadata->numPackets / 1000;
	if(skipCount == 0) skipCount = 1;
	char packetBuffer[PACKET_SIZE];
	PacketData *packet = (PacketData*)packetBuffer;
	while(bitmapRemainder>0){
		unsigned int rcvdLen = SocketWrapper::Recvfrom(sockIn, buf, sizeof(buf), 0, &senderAddr, &senderAddrLen);
		unserialize_Data(buf, rcvdLen, packet);
		if(packet->seqNum == 0xFFFFFFFF){
			continue;   // Duplicate metadata packet
		}
		else if(packet->seqNum < metadata->numPackets){
			// Send ack
			SendAck(packet->seqNum);
			if(bitmap[packet->seqNum] != 1){
				SaveToFile(packet, rcvdLen);
				bitmapRemainder--;
				count++;
				if(count%skipCount == 0)		//Display purposes
					cerr<<".";
			}
			bitmap[packet->seqNum] = 1;
		}
	}
	// Send the termination command
	for(int i=0;i<ACK_TERMINATION_NUMBER; i++)
		SendAck(metadata->numPackets);
	fsync(filefd);
	munmap(mappedPtr, metadata->fileSize);
	close(filefd);
	cerr<<endl;
	
	// Send a confirmation ack packet back
	SendAck(metadata->numPackets);
}

void Receiver::ReceiveMeta(){
	char buf[PACKET_SIZE + 50];   // Static allocation on the stack
	bool isMetaReceived = false;
	PacketMetadata *metadata = new PacketMetadata;
	while(!isMetaReceived){
		socklen_t sendAddrlen = sizeof(struct sockaddr_in);
		ssize_t recvLen;
		recvLen = SocketWrapper::Recvfrom(sockIn, buf, PACKET_SIZE, 0, sendAddr, &sendAddrlen);
		sendAddr->sin_port = htons(SENDER_PORT);
		if(recvLen == sizeof(PacketMetadata)){
			unserialize_Metadata(buf, metadata);
			if(metadata->marker == 0xFFFFFFFF){
				// Valid metadata
				Receiver::metadata = metadata;
				Receiver::bitmap = new char[metadata->numPackets];
				bitmapRemainder = metadata->numPackets;
				memset(Receiver::bitmap, 0, metadata->numPackets);
				InitializeFile();
				// Ensure that sendAddr is redirected to proper port (the sockIn for sender)
				//sendAddr->sin_port = htons(SENDER_PORT);
				isMetaReceived = true;
			}
			else{
				Receiver::SendMetaNack();
			}
		}
		else
			Receiver::SendMetaNack();
	}
}

void Receiver::SendMetaNack(){
	if(sendAddr != NULL){
		PacketAck packet;
		packet.type = TYPE_NACK;
		packet.seqNum = ~0;
		char buf[sizeof(PacketAck)];
		serialize_Ack(&packet, buf);
		while(1){
			try{
				SocketWrapper::Sendto(sockOut, buf, sizeof(PacketAck), 0, sendAddr, sizeof(sockaddr_in));
				//cerr<<"Resent the metadata"<<endl;
			}
			catch(int ex){
				if(ex == EX_SOCK_ERRSENDTO && errno == ENOBUFS){
					continue;
				}
				else {
					throw ex;
				}
			}
			return;
		}
	}
}

void Receiver::InitializeFile(){
	filefd = open(RECVR_FILE, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
	if(filefd == -1)
		throw EX_FILE_NOOPEN;
	if(lseek(filefd, metadata->fileSize-1, SEEK_SET)== -1)
		throw EX_FILE_NOSEEK;
	if(write(filefd, "", 1) == -1)
		throw EX_FILE_NOWRITE;
#ifdef MAP_NOSYNC
	mappedPtr = mmap(0, metadata->fileSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOSYNC, filefd, 0);
#else
	mappedPtr = mmap(0, metadata->fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, filefd, 0);
#endif
	if(mappedPtr == MAP_FAILED)
		throw EX_FILE_NOMAP;
}

void Receiver::SendAck(unsigned int seqNum){
	for(int i=0;i<ACK_RETRANSMIT;i++){
		PacketAck packet;
		packet.type = TYPE_ACK;
		packet.seqNum = seqNum;
		char buf[sizeof(PacketAck)];
		serialize_Ack(&packet, buf);
		while(1){
			try{
			SocketWrapper::Sendto(sockOut, buf, sizeof(PacketAck), 0, sendAddr, sizeof(sockaddr_in));
			}
			catch(int ex){
				if(ex == EX_SOCK_ERRSENDTO && errno == ENOBUFS)
					continue;
				else throw ex;
			}
			return;
		}
	}
}

void Receiver::SaveToFile(PacketData* packet, unsigned int packLen){
	int dataLen = packLen - sizeof(uint32_t);
	int chunkSize = metadata->sizePacket - sizeof(uint32_t);
	unsigned int location = packet->seqNum * chunkSize;
	memcpy((char*)mappedPtr+location, packet->data, dataLen);
}
