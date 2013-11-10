/* 
	Copyright 2013 Mona - mathieu.poux[a]gmail.com
 
	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License received along this program for more
	details (or else see http://www.gnu.org/licenses/).

	This file is a part of Mona.
*/

#include "Mona/WebSocket/WSWriter.h"


using namespace std;


namespace Mona {

WSWriter::WSWriter(StreamSocket& socket) : ping(0),_socket(socket),_sent(0) {
	
}

void WSWriter::close(int type) {
	write(WS_CLOSE, NULL, (UInt32)type);
	Writer::close(type);
}


JSONWriter& WSWriter::newWriter() {
	pack();
	WSSender* pSender = new WSSender();
	_senders.emplace_back(pSender);
	pSender->writer.stream.next(10); // header
	return pSender->writer;
}

void WSWriter::pack() {
	if(_senders.empty())
		return;
	WSSender& sender = *_senders.back();
	if(sender.packaged)
		return;
	JSONWriter& writer = sender.writer;
	writer.end();
	BinaryStream& stream = writer.stream;
	UInt32 size = stream.size()-10;
	UInt32 pos = 10-WS::HeaderSize(size);
	stream.resetWriting(pos);
	stream.resetReading(pos);
	stream.resetWriting(pos+WS::WriteHeader(WS_TEXT,size,writer.writer)+size);
	_sent += stream.size();
	sender.packaged = true;
}

void WSWriter::flush(bool full) {
	if(_senders.empty())
		return;
	pack();
	_qos.add(ping,_sent);
	_sent=0;
	FLUSH_SENDERS("WSSender flush", WSSender, _senders)
}


WSWriter::State WSWriter::state(State value,bool minimal) {
	State state = Writer::state(value,minimal);
	if(state==CONNECTED && minimal)
		_senders.clear();
	return state;
}

void WSWriter::write(UInt8 type,const UInt8* data,UInt32 size) {
	if(state()==CLOSED)
		return;
	WSSender* pSender = new WSSender();
	pSender->packaged = true;
	_senders.emplace_back(pSender);
	BinaryWriter& writer = pSender->writer.writer;
	if(type==WS_CLOSE) {
		// here size is the code!
		if(size>0) {
			WS::WriteHeader(type,2,writer);
			writer.write16(size);
		} else
			WS::WriteHeader(type,0,writer);
		return;
	}
	WS::WriteHeader(type,size,writer);
	writer.writeRaw(data,size);
}



DataWriter& WSWriter::writeInvocation(const std::string& name) {
	if(state()==CLOSED)
        return DataWriter::Null;
	DataWriter& invocation = newWriter();
	invocation.writeString(name);
	return invocation;
}

DataWriter& WSWriter::writeMessage() {
	if(state()==CLOSED)
        return DataWriter::Null;
	return newWriter();
}


bool WSWriter::writeMedia(MediaType type,UInt32 time,MemoryReader& data) {
	if(state()==CLOSED)
		return true;
	switch(type) {
		case START:
			writeInvocation("__publishing").writeString(string((const char*)data.current(),data.available()));
			break;
		case STOP:
			writeInvocation("__unpublishing").writeString(string((const char*)data.current(),data.available()));
			break;
		case DATA: {
			JSONWriter& writer = newWriter();
			writer.writer.write8('[');
			writer.writer.writeRaw(data.current(),data.available());
			writer.writer.write8(']');
			break;
		}
		case INIT:
			break;
		default:
			return Writer::writeMedia(type,time,data);
	}
	return true;
}


} // namespace Mona
