#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "SDMMCBlockDevice.h"
#include "FATFileSystem.h"

WiFiClient wifiClient;                                    //WIFI 클라이언트를 MQTT 클라이언트에 전달
PubSubClient mqttClient(wifiClient);

char ssid[]="SCRC-Research";                              //공유기 SSID
char pass[]="kmuiout@2019";                               //공유기 비밀번호
char fixedTopic[100]="BIOLOGGER/PENGUIN/1/";              //고정 토픽
const char* mqtt_server="192.168.50.191";                 //MQTT 서버 주소
String mqttClientID="SCRC_RFD-";                          //MQTT 클라이언트 아이디
char* cutMessage;                                         //형식별로 자른 수신된 메세지 저장용

SDMMCBlockDevice block_device;                            //MBED 파일 시스템
mbed::FATFileSystem fs("fs");

char fileBuffer[1000];                                    //1줄에 1000바이트 이상시 누락
FILE *fp;                                                 //파일 선언
int number=1;                                             //파일 줄
int toggle=0;                                             //보내야할 파일 시작 줄
String message="";                                        //통합 메세지
String messageFile="";                                    //파일명
String messagePath="";                                    //경로포함 파일명
String messageByte="";                                    //파일 내용
String messageTopic=String(fixedTopic);                   //토픽명
String messageTemp="";                                    //임시 저장 메세지
String messageReady="";                                   //대기중 고정 메세지
char messageChar[1000];                                   //수신한 메세지를 char로 변환

bool startSwitch=false;                                   //전송 시작 스위치
bool stopSwitch=false;                                    //전송중 중지 스위치
bool restartSwitch=false;                                 //재전송 스위치
bool doneSwitch=false;                                    //전송완료후 삭제대기시 유지되는 스위치
bool continueSwitch=false;                                //다음 파일 보내기 작업을 계속하도록 하는 스위치

DIR *dir;                                                 //폴더 선언
struct dirent *ent;                                       //폴더 읽기용 구조체
int dirIndex=0;                                           //읽은 파일 인덱스

void reconnect();                                         //함수 선언
void sendMessage(String stringMessage);
void sendFile();
void sendByte();
void callback(char* topic,byte* payload,unsigned int length);

void setup(){
  Serial.begin(9600);                                     //아두이노
  pinMode(LED_BUILTIN,OUTPUT);                            //초록불
  Serial.println("BOOT");

  int err=fs.mount(&block_device);                        //SD 카드 마운트
  if(err){                                                //SD 카드 없을시
    Serial.println("NO SD CARD");
    Serial.println("RETRY...");
    err=fs.reformat(&block_device);                       //SD 카드 재시도
  }
  if(err){                                                //SD 카드 재시도 실패
    Serial.println("NO SD CARD");
    while(1){digitalWrite(LED_BUILTIN,!digitalRead(LED_BUILTIN));}
  }

  Serial.println("- LIST SD CARD CONTENT -");             //SD 카드 내부 목록
  if((dir=opendir("/fs"))!=NULL){                         //SD 카드 내부 목록 읽기 시도
    while((ent=readdir(dir))!=NULL){                      //읽기 성공시 하나씩 읽기
      Serial.println(ent->d_name);
      dirIndex++;
    }
    closedir(dir);                                        //SD 카드 읽기 해제

  }else{                                                  //SD 카드 내부 목록 읽기 실패시
    Serial.println("ERROR OPENING SD CARD");
  }

  if(dirIndex==0){                                        //SD 카드가 비어있을시
    Serial.println("EMPTY SD CARD");
  }

  while(WiFi.status()!=WL_CONNECTED){                     //WIFI 연결시도
    Serial.println("CONNECTING WIFI");
    WiFi.begin(ssid,pass);
    delay(5000);                                          //5초 대기
  }
  Serial.println("WIFI CONNECTION COMPLETE");

  mqttClient.setServer(mqtt_server,1883);                 //MQTT 서버 세팅
  mqttClient.setCallback(callback);

  int checksum=0;                                         //고유 아이디 만들기용 체크섬 선언
  for(int u=0;u<2048;u++){
    checksum+=*((byte*)u);                                //checksum+=램 바이트 숫자
  }
  mqttClientID+=String(checksum,HEX);                     //고유 아이디 : SCRC_RFD-고유값
  Serial.print("MQTT CLIENT ID : ");
  Serial.println(mqttClientID);

  messageTopic+=mqttClientID;                             //고정 토픽에 고유 아이디값 추가
  messageTopic.toCharArray(fixedTopic,sizeof(fixedTopic));

  if(mqttClient.connect(mqttClientID.c_str())){           //연결 성공시 MQTT CONNECTION COMPLETE 보내고 출력
    mqttClient.subscribe(mqttClientID.c_str());
    mqttClient.publish(fixedTopic,"MQTT CONNECTION COMPLETE");
    Serial.println("MQTT CONNECTION COMPLETE");
  }
}

void loop(){
  digitalWrite(LED_BUILTIN,HIGH);                         //초록불

  if(!mqttClient.connected()){                            //MQTT 서버에 연결 안 되어있을시
    reconnect();                                          //MQTT 연결 시도

  }else{                                                  //MQTT 서버에 연결되어 있을시
    mqttClient.publish(fixedTopic,"START_READY");         //MQTT 서버에 START_READY 메시지 전송
    Serial.println("PUBLISHED MESSAGE : START_READY");
    mqttClient.loop();                                    //MQTT 메세지 수신
    if(startSwitch){                                      //"START:"메세지 수신시 파일보내기 시작
      sendFile();
      startSwitch=false;
    }
    delay(1000);                                          //1초 대기
  }
}

void sendMessage(String stringMessage){                   //String 타입을 MQTT 서버에 전송하고 출력하는 함수
  mqttClient.publish(fixedTopic,stringMessage.c_str());   //MQTT 서버에 전송
  Serial.print("PUBLISHED MESSAGE : ");                   //보낸 내용 출력
  Serial.println(stringMessage);
}

void sendFile(){
  while(mqttClient.connected()){                          //MQTT 서버에 연결되어있다면 계속 보낸다.
    dirIndex=0;                                           //파일 목록 갯수 초기화
    if((dir=opendir("/fs"))!=NULL){                       //SD 카드 내부 목록 읽기 시도
      while((mqttClient.connected())&&((ent=readdir(dir))!=NULL)){//읽기 성공시 하나씩 읽기
        if(strstr(ent->d_name,".")!=NULL){                //파일만 읽기 필터
          messageFile=String(ent->d_name);
          messagePath="fs/"+String(ent->d_name);          //fs/ 경로 합치기
          fp=fopen(messagePath.c_str(),"r");              //파일 열기
          if(fp==NULL){                                   //파일 없을시 넘기기
            Serial.println("NO FILE");
            continue;
          }
          number=1;                                       //줄번호 초기화
          toggle=0;                                       //보내야할 시작 줄 번호 초기화
          message="";                                     //메세지 초기화

          sendByte();                                     //파일을 한줄씩 보내는 함수

          dirIndex++;                                     //파일 인덱스 증가

          if((!mqttClient.connected())||(stopSwitch)){    //중지중 파일을 다시 부르기 실패 -재전송 실패시 다음 파일 보내기
            stopSwitch=false;
            continue;
          }

          message=messageFile+":"+number+":DONE";         //파일명:줄번호:DONE -파일을 전부 보냈음을 알림
          sendMessage(message);                           //메세지 전송

          fclose(fp);                                     //파일 닫기

          messageReady=messageFile+":DELETE_READY";       //파일명:DELETE_READY -삭제 대기 메세지

          continueSwitch=false;                           //컨티뉴 스위치 초기화
          doneSwitch=true;                                //삭제대기가 끝날때까지 스위치 유지
          while(mqttClient.connected()){
            mqttClient.loop();                            //MQTT 메세지 수신
            if(continueSwitch){                           //컨티뉴 스위치 활성화 -대기 해제
              continueSwitch=false;                       //컨티뉴 스위치 초기화
              break;
            }else if(restartSwitch){                      //재전송 스위치 활성화 -재전송 시작
              restartSwitch=false;                        //재전송 스위치 초기화
              fp=fopen(messagePath.c_str(),"r");          //파일 열기
              if(fp==NULL){                               //파일 없을시 넘기기
                Serial.println("NO FILE");
                mqttClient.publish(fixedTopic,"NO FILE");
                break;
              }else{
                number=1;                                 //줄번호 초기화
                message="";                               //메세지 초기화
                sendByte();                               //파일을 한줄씩 보내는 함수
                fclose(fp);                               //파일 닫기
                break;
              }
            }
            sendMessage(messageReady);                    //삭제 대기 메세지 전송
            delay(1000);                                  //1초 대기
          }
          doneSwitch=false;                               //던 스위치 초기화
        }
      }
      closedir(dir);                                      //SD 카드 읽기 해제

    }else{                                                //SD 카드 내부 목록 읽기 실패시
      Serial.println("ERROR OPENING SD CARD");
      mqttClient.publish(fixedTopic,"ERROR OPENING SD CARD");//MQTT 서버에 전송
    }
    if(dirIndex==0){                                      //SD 카드가 비어있을시
      Serial.println("EMPTY SD CARD");
      mqttClient.publish(fixedTopic,"EMPTY SD CARD");     //MQTT 서버에 전송
    }
    delay(5000);                                          //5초 대기
  }
}

void sendByte(){
  while(mqttClient.connected()){                          //파일 하나를 다 보낼때까지 무한 반복
    fgets(fileBuffer,sizeof(fileBuffer),fp);
    if(feof(fp)){break;}                                  //파일 하나를 다 읽었으면 반복문 탈출

    if(number>=toggle){                                   //지정된 줄 수 부터 보냄
      messageByte=String(fileBuffer);
      message=messageFile+":"+number+":"+messageByte;     //파일명:줄번호:파일내용
      sendMessage(message);                               //메세지 전송
    }

    number++;                                             //줄번호 증가

    mqttClient.loop();                                    //메세지 수신

    if(stopSwitch){                                       //정지 메세지 수신시
      while(mqttClient.connected()){
        message=messageFile+":OK";                        //파일명:OK
        sendMessage(message);                             //메세지 전송
        mqttClient.loop();
        if(restartSwitch){                                //재전송 메세지 수신시
          restartSwitch=false;
          break;
        }
        delay(1000);                                      //1초 대기
      }
      fclose(fp);                                         //파일 닫기
      fp=fopen(messagePath.c_str(),"r");                  //파일 새로 열기
      if(fp==NULL){                                       //파일 없을시 넘기기
        Serial.println("NO FILE");
        mqttClient.publish(fixedTopic,"NO FILE");
        break;
      }else{                                              //파일 있을시 재전송 시작
        number=1;                                         //줄번호 초기화
        message="";                                       //메세지 초기화
        stopSwitch=false;                                 //정지 스위치 초기화
        continue;
      }
    }
  }
}

void reconnect(){
  while(!mqttClient.connected()){                         //MQTT 서버에 연결될 때까지 반복
    while (WiFi.status()!=WL_CONNECTED) {                 //WIFI가 문제일수도 있으므로 WIFI가 끊겨있을시 연결될때까지 반복
      Serial.println("CONNECTING WIFI");
      WiFi.begin(ssid,pass);
      delay(5000);                                        //5초 대기
    }
    Serial.println("WIFI CHECK COMPLETE");
    Serial.println("CONNECTING MQTT");
    if(mqttClient.connect(mqttClientID.c_str())){         //연결 성공시 MQTT CONNECTION COMPLETE 보내고 출력
      mqttClient.subscribe(mqttClientID.c_str());
      mqttClient.publish(fixedTopic,"MQTT CONNECTION COMPLETE");
      Serial.println("MQTT CONNECTION COMPLETE");
    }else{                                                //연결 실패시 현상태 출력
      Serial.print("FAILED : ");
      Serial.println(mqttClient.state());
      Serial.println("TRY AGAIN IN 5 SECONDS");
      delay(5000);                                        //5초 대기
    }
  }
}

void callback(char* topic,byte* payload,unsigned int length){//메세지 수신
  Serial.print("CONTROL MESSAGE : ");
  message="";                                             //메세지 초가화
  for(int i=0;i<length;i++){
    message+=String((char)payload[i]);
  }
  Serial.println(message);                                //수신 메세지 출력

  message.toCharArray(messageChar,sizeof(messageChar));   //CHAR로 변환
  cutMessage=strtok(messageChar,":");                     //:로 구분해서 명령어 나누기

  if(cutMessage!=NULL){                                   //:로 나뉠시

    if(strcmp(cutMessage,"START")==0){                    //START 명령어 수신시
      startSwitch=true;                                   //시작 스위치 활성화
      return;                                             //함수종료

    }else if(strcmp(cutMessage,"STOP")==0){               //STOP 명령어 수신시
      stopSwitch=true;                                    //정지 스위치 활성화
      return;                                             //함수종료

    }else if(strcmp(cutMessage,"RESTART")==0){            //RESTART 명령어 수신시
      if(!(stopSwitch||doneSwitch)){return;}              //중복방지 정지중이거나 전부 전송후 삭제 대기중이 아닐시 누락

      cutMessage=strtok(NULL,":");                        //:로 구분된 RESTART:의 다음 구문
      if(cutMessage!=NULL){                               //:로 나뉠시
        messageTemp=String(cutMessage);                   //재전송할 파일명 임시 메세지에 저장

        cutMessage=strtok(NULL,":");                      //:로 구분된 RESTART:파일명:의 다음 구문
        if(cutMessage!=NULL){                             //:로 나뉠시
          toggle=atoi(cutMessage);                        //어디부터 보내야 할 지에 대한 줄 번호 저장

          if(toggle==0){                                  //재전송 요청 명령이 숫자가 아니거나 0일시
            Serial.println("INCORRECT FILE LINE");
            mqttClient.publish(fixedTopic,"INCORRECT FILE LINE");
            continueSwitch=true;                          //잘못된 재전송 요청이므로 무시하고 컨티뉴 스위치 활성화
            return;                                       //함수종료
          }

          messageFile=messageTemp;                        //정상 명령어일시 임시 메세지에서 파일명에 저장
          messagePath="fs/"+messageTemp;                  //경로 fs/ 붙이기
          restartSwitch=true;                             //재전송 스위치 활성화
          return;                                         //함수종료

        }else{                                            //RESTART:파일명: 뒤에 아무것도 없을시
          Serial.println("NO FILE LINE");
          mqttClient.publish(fixedTopic,"NO FILE LINE");
          continueSwitch=true;                            //잘못된 재전송 요청이므로 무시하고 컨티뉴 스위치 활성화
          return;                                         //함수종료
        }

      }else{                                              //RESTART: 뒤에 아무것도 없을시
        Serial.println("INCORRECT RESTART MESSAGE");
        mqttClient.publish(fixedTopic,"INCORRECT RESTART MESSAGE");//MQTT 서버에 전송
        continueSwitch=true;                              //잘못된 재전송 요청이므로 무시하고 컨티뉴 스위치 활성화
        return;                                           //함수종료
      }

    }else if(strcmp(cutMessage,"DELETE")==0){             //DELETE 명렁어 수신시
      if(!doneSwitch){return;}                            //중복방지 전부 전송후 삭제 대기중이 아닐시 누락
      continueSwitch=true;                                //실행결과에 상관없이 상대쪽이 다 받았다는 것이므로 컨티뉴 스위치 활성화

      cutMessage=strtok(NULL,":");                        //:로 구분된 DELETE:의 다음 구문
      if(cutMessage!=NULL){                               //:로 나뉠시
        messageFile=String(cutMessage);
        messagePath="fs/"+String(cutMessage);             //경로 fs/ 붙이기

        number=remove(messagePath.c_str());               //파일 삭제

        if(number==0){                                    //파일 삭제 성공시
          Serial.print(messagePath);
          Serial.println(" : DELETE COMPLETE");
          message=messageFile+" - DELETE COMPLETE";       //파일명 - DELETE COMPLETE
          sendMessage(message);                           //메세지 전송
          return;                                         //함수종료

        }else{                                            //파일 삭제 실패시
          Serial.print(messagePath);
          Serial.println(" : DELETE FAIL");
          message=messageFile+" - DELETE FAIL";           //파일명 - DELETE FAIL
          sendMessage(message);                           //메세지 전송
          return;                                         //함수종료
        }

      }else{                                              //DELETE: 뒤에 아무것도 없을시
        Serial.println("INCORRECT DELETE MESSAGE");
        mqttClient.publish(fixedTopic,"INCORRECT DELETE MESSAGE");//MQTT 서버에 전송
        return;                                           //함수종료
      }

    }else{                                                //일치하는 명령어가 없거나 잘못된 형식의 명령어 수신시
      Serial.println("INCORRECT CONTROL MESSAGE");
      mqttClient.publish(fixedTopic,"INCORRECT CONTROL MESSAGE");//MQTT 서버에 전송
      return;                                             //함수종료
    }

  }else{                                                  //:로 구분된 명령어가 아닐시
    Serial.println("NO CONTROL MESSAGE");
    mqttClient.publish(fixedTopic,"NO CONTROL MESSAGE");  //MQTT 서버에 전송
    return;                                               //함수종료
  }
}
