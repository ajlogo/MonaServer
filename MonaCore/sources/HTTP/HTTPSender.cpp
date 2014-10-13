/*
Copyright 2014 Mona
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

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

#include "Mona/HTTP/HTTPSender.h"
#include "Mona/FileSystem.h"
#include "Mona/StringWriter.h"
#include "Mona/MIME.h"
#include "Mona/Logs.h"
#include "Mona/HTTP/HTTPWriter.h"

#include <fstream>

using namespace std;


namespace Mona {



HTTPSender::HTTPSender(const SocketAddress& address, HTTPPacket& request,const PoolBuffers& poolBuffers, const string& relativePath) :
	_serverAddress(request.serverAddress),
	_ifModifiedSince(request.ifModifiedSince),
	_connection(request.connection),
	_command(request.command),
	_pInfos(request.pullSendingInfos()),
	_address(address),
	_sizePos(0),
	_sortOptions(0),
	_isApp(false),
	_poolBuffers(poolBuffers),
	_appPath(relativePath),
	TCPSender("TCPSender") {

}

void HTTPSender::onSent(Socket& socket) {
	if (_connection != HTTP::CONNECTION_CLOSE)
		return;
	// disconnect socket if _connection==HTTP::CONNECTION_CLOSE
	Exception ex;
	socket.shutdown(ex,Socket::SEND);
}

bool HTTPSender::run(Exception& ex) {

	if (!_pWriter) {

		//// GET FILE
		Date date;
		// Not Modified => don't send the file
		if (_file.lastModified()>0 && _ifModifiedSince >= _file.lastModified()) {
			write("304 Not Modified", HTTP::CONTENT_ABSENT);
		} else {
			// file doesn't exist
			if (_file.lastModified()==0)
				writeError(404, "The requested URL ", _file.toString(), " was not found on this server");
			// Folder
			else if (_file.isDirectory()) {
				// Connected to parent => redirect to url + '/'
				if (!_isApp) {
					// Redirect to the real path of directory
					BinaryWriter& writer(write("301 Moved Permanently").packet);
					String::Format(_buffer, "http://", _serverAddress, _appPath, '/');
					HTTP_BEGIN_HEADER(writer)
						HTTP_ADD_HEADER("Location", _buffer)
					HTTP_END_HEADER
					HTML_BEGIN_COMMON_RESPONSE(writer, EXPAND("Moved Permanently"))
						writer.write(EXPAND("The document has moved <a href=\"")).write(_buffer).write(EXPAND("\">here</a>."));
					HTML_END_COMMON_RESPONSE(_buffer)
				} else {
					BinaryWriter& writer(write("200 OK", HTTP::CONTENT_TEXT, "html; charset=utf-8").packet);
					HTTP_BEGIN_HEADER(writer)
						HTTP_ADD_HEADER("Last-Modified", date.toString(Date::HTTP_FORMAT, _buffer))
					HTTP_END_HEADER

					HTTP::WriteDirectoryEntries(writer, _serverAddress, _file.toString(), _appPath, _sortOptions);
				}
			} 
			// File
			else {
#if defined(_WIN32)
				wchar_t wFile[_MAX_PATH];
				MultiByteToWideChar(CP_UTF8, 0, _file.toString().c_str(), -1, wFile, _MAX_PATH);
				ifstream ifile(wFile, ios::in | ios::binary | ios::ate);
#else
				ifstream ifile(_file.toString(), ios::in | ios::binary | ios::ate);
#endif
				if (!ifile.good())
					writeError(423, "Impossible to open ", _appPath, "/", _file.name(), " file");
				else {
					// determine the content-type
					string subType;
					HTTP::ContentType type = HTTP::ExtensionToMIMEType(_file.extension(), subType);	

					PacketWriter& response(write("200 OK", type,subType.c_str()).packet);
					HTTP_BEGIN_HEADER(response)
						HTTP_ADD_HEADER("Last-Modified", date.toString(Date::HTTP_FORMAT, _buffer))
					HTTP_END_HEADER

					// TODO see if filter is correct
					if (type == HTTP::CONTENT_TEXT && _pInfos && _pInfos->parameters.count())
						replaceTemplateTags(response, ifile, _pInfos->parameters, _pInfos->sizeParameters);
					else {

						// push the entire file content to memory
						UInt32 size = (UInt32)ifile.tellg();
						ifile.seekg(0);
						char* current = (char*)response.buffer(size); // reserve memory for file
						ifile.read(current, size);
					}
				}
			}
		}
	
	}
	
	/// Write Content-Length if position recorded
	if (_sizePos > 0) {
		PacketWriter& packet(_pWriter->packet);
		UInt32 size = packet.size();

		// search \r\n\r\n of header part
		const UInt8* end = packet.data()+size-4;
		const UInt8* content = packet.data()+_sizePos+9;
		while (content < end) {
			if (memcmp(++content, EXPAND("\r\n\r\n")) == 0) {
				content += 4;
				break;
			}
			if (content == end)
				ERROR("HTTP header without end, unvalid packet")
		}

		// write content-length
		String::Format(_buffer, end + 4 - content);
		memcpy((UInt8*)packet.data()+_sizePos,_buffer.c_str(),_buffer.size());

		if (_command == HTTP::COMMAND_HEAD)
			packet.clear(content-packet.data());
		else
			packet.clear(size);
	}

	/// Dump response
	Session::DumpResponse(data(), size(), _address);

	/// Send
	return TCPSender::run(ex);
}

DataWriter& HTTPSender::writer(const string& code, HTTP::ContentType type, const char* subType, const UInt8* data, UInt32 size) {
	if (_pWriter) {
		ERROR("HTTP response already written");
		return DataWriter::Null;
	}

	if (type == HTTP::CONTENT_ABSENT || data || size || !subType || !MIME::CreateDataWriter(subType, _poolBuffers, _pWriter))
		_pWriter.reset(new StringWriter(_poolBuffers));

	PacketWriter& packet = _pWriter->packet;
	packet.clear();

	// First line (HTTP/1.1 200 OK)
	UInt16 value(200);
	packet.write(EXPAND("HTTP/1.1 "));
	packet.write(code);
	if (String::ToNumber<UInt16>(code, value)) {
		packet.write(" ");
		packet.write(HTTP::CodeToMessage(value));
	}

	// Date + Mona
	packet.write(EXPAND("\r\nDate: ")).write(Date().toString(Date::HTTP_FORMAT, _buffer));
	packet.write(EXPAND("\r\nServer: Mona"));

	// Connection type, same than request!
	if (_connection&HTTP::CONNECTION_KEEPALIVE) {
		packet.write(EXPAND("\r\nConnection: keep-alive"));
		if (_connection&HTTP::CONNECTION_UPGRADE)
			packet.write(EXPAND(", upgrade"));
		if (_connection&HTTP::CONNECTION_CLOSE)
			packet.write(EXPAND(", close"));
	} else if (_connection&HTTP::CONNECTION_UPGRADE) {
		packet.write(EXPAND("\r\nConnection: upgrade"));
		if (_connection&HTTP::CONNECTION_CLOSE)
			packet.write(EXPAND(", close"));
	} else if (_connection&HTTP::CONNECTION_CLOSE)
		packet.write(EXPAND("\r\nConnection: close"));

	// Set Cookies
	if (_pInfos) {
		for(const string& cookie : _pInfos->setCookies)
			packet.write(EXPAND("\r\nSet-Cookie: ")).write(cookie);
	}

	// Content Type
	if (type != HTTP::CONTENT_ABSENT) {
		packet.write(EXPAND("\r\nContent-Type: "));
		packet.write(HTTP::FormatContentType(type, subType, _buffer));
		
		// Content Length
		if (data) {
			packet.write(EXPAND("\r\nContent-Length: "));
			packet.write(String::Format(_buffer, size));
		} else if (size == 1) { // if size==1 means that we want not writing a content-length
			// here it means that we are on a live streaming, without size limit, so we have to signal the cache-control
			packet.write(EXPAND("\r\nCache-Control: no-cache, no-store\r\nPragma: no-cache"));
		} else {
			// reserve place to add length on sending
			packet.write(EXPAND("\r\nContent-Length:           "));
			_sizePos = packet.size()-10;
		}
	}

	packet.write("\r\n\r\n");
	if (data && size > 0)
		packet.write(data, size);
	if(data)
		return DataWriter::Null;
	_pWriter->clear(packet.size());
	return *_pWriter;
}

BinaryWriter& HTTPSender::writeRaw(const PoolBuffers& poolBuffers) {
	if (_pWriter) {
		ERROR("HTTP response already written");
		return DataWriter::Null.packet;
	}
	_pWriter.reset(new StringWriter(poolBuffers));
	return _pWriter->packet;
}

void HTTPSender::replaceTemplateTags(PacketWriter& packet, ifstream& ifile, const Parameters& parameters, UInt32 sizeParameters) {

	UInt32 pos = packet.size();
	// get file content size
	UInt32 size = (UInt32)ifile.tellg();

	// push the entire file content to memory
	ifile.seekg(0);
	char* current = (char*)packet.buffer(size+sizeParameters); // reserve more memory to change <%name%> field
	ifile.read(current, size);
	// iterate on content to replace "<% key %>" fields
	const char* begin = current;
	const char* end = current + size;
	UInt8 step(0);
	char* signifiant(NULL);
	UInt32 keyLength(0);
	const char* keyBegin(NULL);
	string key;
	while (current < end) {
		char c = *current;
		if (step < 2) {
			if (step==0 && c == '<')
				++step;
			else if (step == 1 && c == '%') {
				++step;
				signifiant = current-1;
				keyBegin = NULL;
				keyLength = 0;
				key.clear();
			} else
				step = 0;
		} else {
			// in <% ... %>
			if (step==2 && c == '%')
				++step;
			else if (step==3 && c == '>')
				++step;
			else {
				step = 2;
				// search key
				if (key.empty()) {
					if (!isspace(c)) {
						if (!keyBegin)
							keyBegin = current;
						++keyLength;
					} else if(keyBegin)
						key.assign(keyBegin, keyLength);
				}
			}

			if (step == 4) {
				step = 0;

				if (keyBegin)
					key.assign(keyBegin, keyLength);
				UInt32 available(current+1-signifiant);
				string value;
				parameters.getString(key, value);
				// give the size available required
				if (available < value.size()) {
					available = value.size()-available; // to add
					memmove(current+1+available,current+1,end-current-1);
					end += available;
					current += available-1;
				} else if (available>value.size()) {
					available = available-value.size(); // to remove
                    memmove(current+1-available,current+1,end-current-1);
					end -= available;
					current -= available-1;
				}
				// replace <% key %> by value
				if (!value.empty())
					memcpy(signifiant,value.c_str(),value.size());
			}
		}
		++current;
	}

	// resize final stream
	packet.clear(pos+(end-begin));
}

} // namespace Mona
