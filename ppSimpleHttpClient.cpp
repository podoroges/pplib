#include <winsock2.h>
#include <ws2tcpip.h>
#include "ppSimpleHttpClient.h"
#pragma link "ws2_32.lib"
#pragma comment(lib, "ws2_32.lib")

//---------------------------------------------------------------------------
// unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
// https://tools.ietf.org/html/rfc3986
void UrlEncode(AnsiString &st){
  AnsiString result;
  for(int a=1;a<=st.Length();a++){
    int unreserved = 0;
    unsigned char c = st[a];
    if((c>=48)&&(c<=57)) // 0..9
      unreserved = 1;
    if((c>=65)&&(c<=90)) // A-Z
      unreserved = 1;
    if((c>=97)&&(c<=122))// a-z
      unreserved = 1;
    if((c=='-')||(c=='.')||(c=='_')||(c=='~'))
      unreserved = 1;
    if(!unreserved){
      st[a] = '%';
      st.Insert(AnsiString().sprintf("%02X",c),a+1);
      a = a+2;
    }
  }
}
//---------------------------------------------------------------------------
//  ExtractString1("archive1.zip","arc",".zip") = "hive1"
AnsiString ppSimpleHttpClient::ExtractString1(AnsiString source,AnsiString start,AnsiString end){
  if(!source.Pos(start))return "";
  source = source.Delete(1,source.Pos(start)+start.Length()-1);
  if(!source.Pos(end))return source;
  source = source.Delete(source.Pos(end),source.Length());
  return source;
}
//---------------------------------------------------------------------------
__fastcall ppSimpleHttpClient::ppSimpleHttpClient(): TThread(1){
  HeadersParsed = 0;
  FreeOnTerminate = 1;
  Output = NULL;
  Content = NULL;
}
//---------------------------------------------------------------------------
__fastcall ppSimpleHttpClient::~ppSimpleHttpClient(){
  delete Output;
  delete Content;
  Log((AnsiString)"Thread ["+int(this)+"] released.");
}
//---------------------------------------------------------------------------
void ppSimpleHttpClient::Get(AnsiString url){
  Url = url;
  Method = "GET";
  Resume();
}
//---------------------------------------------------------------------------
void ppSimpleHttpClient::PostSimpleForm(AnsiString url,AnsiString fields){
  Url = url;
  Method = "POST";
  ContentType = "application/x-www-form-urlencoded";
  AnsiString result;
  TStringList * str = new TStringList();
  str->Text = fields;
  for(int a=0;a<str->Count;a++){
    AnsiString f = str->Names[a];
    AnsiString v = str->Values[f];
    UrlEncode(f);
    UrlEncode(v);
    if(a>0)
      result = (AnsiString)result+"&";
    result = (AnsiString)result+f+"="+v;
  }
  delete str;
  Content = new TStringStream(result);
  Resume();
}
//---------------------------------------------------------------------------
void ppSimpleHttpClient::PostSingleFile(AnsiString url,AnsiString fname,AnsiString field){
  Url = url;
  Method = "POST";
  // TODO: The encapsulation boundary MUST NOT appear inside any of the encapsulated parts.
  // Thus, it is crucial that the composing agent be able to choose and specify the unique
  // boundary that will separate the parts.
  // https://www.w3.org/Protocols/rfc1341/7_2_Multipart.html
  AnsiString boundary = "--ppSimpleHttpClientFormBoundary";
  ContentType = (AnsiString)"multipart/form-data; boundary="+boundary;
  TStringList * str = new TStringList();
  // The encapsulation boundary is defined as a line consisting entirely of two hyphen
  // characters ("-", decimal code 45) followed by the boundary parameter value from the
  // Content-Type header field.
  str->Add((AnsiString)"--"+boundary);
  str->Add((AnsiString)"Content-Disposition: form-data; name=\""+field+"\"; filename=\""+ExtractFileName(fname)+"\"");
  str->Add("Content-Type: application/octet-stream");
  str->Add("");
  TStringStream * s1 = new TStringStream(str->Text);
  str->Clear();
  str->Add("");
  str->Add((AnsiString)"--"+boundary);
  TStringStream * s2 = new TStringStream(str->Text);
  delete str;
  TFileStream * fs = new TFileStream(fname,fmOpenRead|fmShareDenyNone);
  // Content = s1 + file + s2
  Content = new TMemoryStream();
  Content->CopyFrom(s1,0);
  Content->CopyFrom(fs,0);
  Content->CopyFrom(s2,0);
  delete s1;
  delete s2;
  delete fs;
  Resume();
}
//---------------------------------------------------------------------------
void __fastcall ppSimpleHttpClient::Execute(){
  // 1. Trying to parse Url: {Service}://{Server}/{Path}
  AnsiString Service = "http";
  AnsiString Server = Url;
  AnsiString Path = "/";
  int p1 = Url.Pos("://");
  if(p1>0){
    Service = Url.SubString(1,p1-1);
    Server = Url.SubString(p1+3,Url.Length());
  }
  p1 = Server.Pos("/");
  if(p1>0){
    Path = Server.SubString(p1,Server.Length());
    Server = Server.SubString(1,p1-1);
  }
  Log((AnsiString)"Service:["+Service+"]");
  Log((AnsiString)"Server:["+Server+"]");
  Log((AnsiString)"Path:["+Path+"]");


  struct addrinfo hints;
  struct addrinfo *result, *ptr;
  SOCKET Socket;

  WSADATA wsaData;
  if(WSAStartup((2,2), &wsaData) != 0)
    Log((AnsiString)"WSAStartup() failed: "+WSAGetLastError());

  // 2. Resolve hostname
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
  hints.ai_protocol = 0;          // Any protocol

  int res = getaddrinfo(Server.c_str(), Service.c_str(), &hints, &result);
  if(res!=0){
    if(res==11001)
      Log((AnsiString)"HOST_NOT_FOUND: "+Url);
    else
      Log((AnsiString)"Error getaddrinfo: "+ res);
  }
  else{
 /* getaddrinfo() returns a list of address structures.
     Try each address until we successfully connect(2).
     If socket(2) (or connect(2)) fails, we (close the socket
     and) try the next address. */

    for(ptr = result; ptr != NULL; ptr = ptr->ai_next) {
      Socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
      if(Socket == -1)
        continue;

      if(connect(Socket, ptr->ai_addr, ptr->ai_addrlen) != -1)
        break; // Success

      closesocket(Socket);
    }

    // Create Output stream here, so even in case of "Cannot connect to host"
    // we will not get NULL ptr error in DownloadReady handler.
    Output = new TMemoryStream();
    
    if (ptr == NULL) { // No address succeeded
      Log("Cannot connect to host");
    }
    else{

      struct sockaddr_in * sockaddr_ipv4 = (struct sockaddr_in *) ptr->ai_addr;
      Log((AnsiString)"Connected to "+inet_ntoa(sockaddr_ipv4->sin_addr));

      // 3. Send Http Request
      TStringList * headers = new TStringList;
      headers->Add((AnsiString)Method+" "+Path+" HTTP/1.1");
      if(ContentType.Length())
        headers->Add((AnsiString)"Content-Type: "+ContentType);
      if(Content)
        headers->Add((AnsiString)"Content-Length: "+Content->Size);
      headers->Add((AnsiString)"Host: "+Server);
      headers->Add("");
      TMemoryStream * RequestStream = new TMemoryStream();
      RequestStream->Write(headers->Text.c_str(),headers->Text.Length());
      if(Content)
        RequestStream->CopyFrom(Content,0);
      delete headers;
      int HttpRequestLength = RequestStream->Size;
      char * HttpRequestBuf = (char *)malloc(HttpRequestLength);
      RequestStream->Position = 0;
      RequestStream->Read(HttpRequestBuf,HttpRequestLength);
      delete RequestStream;

      if(send( Socket, HttpRequestBuf, HttpRequestLength, 0 )== SOCKET_ERROR)
        Log((AnsiString)"Error send: "+ WSAGetLastError());
      free(HttpRequestBuf);


      if (shutdown(Socket, SD_SEND) == SOCKET_ERROR)
        Log((AnsiString)"Error shutdown: "+ WSAGetLastError());


      Log("HttpRequest sent.");

      // 4. Read response
        const int buflen = 0x40000;
        char buf[buflen];

      TMemoryStream * ms = new TMemoryStream();
      int iResult;
      do {

        iResult = recv(Socket, buf, buflen, 0);
        if ( iResult > 0 ){
          Log((AnsiString)"Bytes received: "+ iResult+" Size: "+ms->Size);
          if(!HeadersParsed)
            ParseHTTPHeaders(ms);
          if((HeadersParsed)&&(ContentLength>0)){
            ProgressPercent = 100*(ms->Size-ContentStart)/ContentLength;
            if(ms->Size-ContentStart==ContentLength)
              ProgressPercent = 100;
            Synchronize(Progress);
          }


          ms->Write(buf,iResult);
        }
        else if ( iResult == 0 ){
          Log((AnsiString)"Connection closed.");
        }
        else{
          int Error = WSAGetLastError();
          AnsiString ErrorST = Error;
          if(Error == 10054)ErrorST = "Connection reset by peer";
          Log((AnsiString)"recv failed with error: "+ErrorST);
          break;
        }

      } while( iResult > 0);
      // 5. Parse result
      if(!HeadersParsed)
        ParseHTTPHeaders(ms);
      if(HeadersParsed){
        ms->Position = ContentStart;
        Output->CopyFrom(ms,ms->Size-ms->Position);
      }
      else{
        // Cannot find headers, take all stream
        Output->CopyFrom(ms,0);
      }
      delete ms;
    }
    freeaddrinfo(result);
    closesocket(Socket);
  }
  WSACleanup();
  Log((AnsiString)"Thread ["+int(this)+"] finished.");
  Synchronize(DownloadReady);
}
//---------------------------------------------------------------------------
void ppSimpleHttpClient::ParseHTTPHeaders(TStream * ms){
  TStringStream * ss = new TStringStream("");
  ss->CopyFrom(ms,0);
  if((ss->DataString.Pos("HTTP")==1)&&(ss->DataString.Pos("\r\n\r\n"))){
    Log("Http headers found.");
    Headers = ss->DataString.SubString(1,ss->DataString.Pos("\r\n\r\n")).Trim();
    ContentType = ExtractString1(Headers,"Content-Type:","\r\n").Trim();
    ContentLength = ExtractString1(Headers,"Content-Length:","\r\n").Trim().ToIntDef(0);
    ContentStart = ss->DataString.Pos("\r\n\r\n")+3; //+4-1
    Log((AnsiString)"ContentType "+ContentType);
    Log((AnsiString)"ContentLength "+ContentLength);
    HeadersParsed = 1;
  }
  delete ss;
}
//---------------------------------------------------------------------------
