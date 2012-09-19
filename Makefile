all: sender receiver

sender: sender.o socketwrapper.o serializer.o
	g++ -g -Wall -o sender sender.o socketwrapper.o serializer.o

receiver: receiver.o socketwrapper.o serializer.o
	g++ -g -Wall -o receiver receiver.o socketwrapper.o serializer.o
	
sender.o: sender.cpp sender.h packetformat.h settings.h exceptions.h socketwrapper.h serializer.h
	g++ -g -c -Wall -o sender.o sender.cpp
	
socketwrapper.o: socketwrapper.cpp socketwrapper.h exceptions.h
	g++ -g -c -Wall -o socketwrapper.o socketwrapper.cpp
	
serializer.o: serializer.cpp serializer.h packetformat.h
	g++ -g -c -Wall -o serializer.o serializer.cpp

receiver.o: receiver.cpp serializer.h packetformat.h receiver.h settings.h socketwrapper.h exceptions.h
	g++ -g -c -Wall -o receiver.o receiver.cpp

clean:
	rm -rf *.o
	rm sender
	rm receiver
	