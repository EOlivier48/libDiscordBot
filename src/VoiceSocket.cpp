/*
 * MIT License
 *
 * Copyright (c) 2020 Christian Tost
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "VoiceSocket.hpp"
#include "Payload.hpp"
#include <Log.hpp>
#include <opus/opus.h>
#include <sodium.h>
#include <time.h>
#include <stdlib.h>

namespace DiscordBot
{
    bool IsLittleEndian()
    {
        short t = 1;
        return ((char*)&t)[0];
    }

    short ChangeEndianess(short Val)
    {
        return ((Val << 8) & 0xFF00) | ((Val >> 8) & 0xFF);
    }

    int ChangeEndianess(int Val)
    {
        return ((Val << 24) & 0xFF000000) | ((Val >> 24) & 0xFF) | ((Val << 8) & 0x00FF0000) | ((Val >> 8) & 0xFF00);
    }

    /**
     * @brief Connects to the gateway.
     */
    CVoiceSocket::CVoiceSocket(CJSON &json, const std::string &SessionID, const std::string &ClientID) : m_Terminate(false), m_HeartACKReceived(false), m_LastSeqNum(-1), m_Stop(true), m_Reconnect(false)
    {
        m_Token = json.GetValue<std::string>("token");
        m_GuildID = json.GetValue<std::string>("guild_id");
        m_SessionID = SessionID;
        m_ClientID = ClientID;

        std::string URL = json.GetValue<std::string>("endpoint");
        size_t Pos = URL.find(":");
        URL = URL.substr(0, Pos);

        m_Socket.setUrl("wss://" + URL + "/?v=4");
        m_Socket.setOnMessageCallback(std::bind(&CVoiceSocket::OnWebsocketEvent, this, std::placeholders::_1));
        m_Socket.start();
    }

    void CVoiceSocket::StartSpeaking(AudioSource Source)
    {
        if(m_SecKey.empty())
        {
            m_Source = Source;
            return;
        }

        if(!m_Stop)
            StopSpeaking();

        m_Pause = false;
        m_Stop = false;

        m_Source = Source;

        SetSpeaking(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        m_Playback = std::thread(&CVoiceSocket::Playback, this);
    }

    void CVoiceSocket::PauseSpeaking()
    {
        m_Pause = true;
        SetSpeaking(true);
    }

    void CVoiceSocket::ResumeSpeaking()
    {
        SetSpeaking(true);
        m_Pause = false;
    }

    void CVoiceSocket::StopSpeaking()
    {
        m_Stop = true;
        if(m_Playback.joinable())
            m_Playback.join();

        SetSpeaking(false);
        m_Source = nullptr;
    }

    void CVoiceSocket::SetSpeaking(bool Speak)
    {
        CJSON json;

        if(Speak)
            json.AddPair("speaking", 5);
        else
            json.AddPair("speaking", 0);

        json.AddPair("delay", 0);
        json.AddPair("ssrc", m_SSRC);

        SendOP(OPCodes::SPEAKING, json.Serialize());
    }

    void CVoiceSocket::Playback()
    {
        if (sodium_init() < 0) 
        {
            llog << lerror << "Error to init libsodium" << lendl;
            return;
        }

        const int FREQUENCY = 48000;
        const int CHANNEL = 2;
        const int MILLISECONDS = 20;

        int err;
        OpusEncoder *Encoder = opus_encoder_create(FREQUENCY, CHANNEL, OPUS_APPLICATION_VOIP, &err);
        if(err)
        {
            llog << lerror << "Error to create opus encoder" << lendl;
            return;
        }

        size_t Size = FREQUENCY * CHANNEL * MILLISECONDS / 1000;   //20 Milliseconds.
        uint16_t *Buf = new uint16_t[Size];
        uint8_t *OpusBuf = new uint8_t[Size];

        srand(time(nullptr));
        uint16_t Seq = 0;//rand() % 1024;
        const int RTPHeaderSize = 12;
        const int NonceSize = RTPHeaderSize * 2;
        int Timestamp = 0;

        int64_t LastSendTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        float LastSpeachTime = MILLISECONDS;

        while (!m_Stop)
        {
            if(m_Pause)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            uint32_t Ret = m_Source->OnRead(Buf, Size / 2);
            opus_int32 OpusSize = opus_encode(Encoder, (opus_int16*)Buf, Size / 2, OpusBuf, Size);
            if(OpusSize > 0)
            {
                ++Seq;

                std::string Data(RTPHeaderSize + OpusSize + crypto_secretbox_MACBYTES, '\0');
                Data[0] = 0x80;
                Data[1] = 0x78;

                uint16_t SeqBig = Seq;
                int TimestampBig = Timestamp;
                int SSRCBig = m_SSRC;

                if(IsLittleEndian())
                {
                    SeqBig = ChangeEndianess(SeqBig);
                    TimestampBig = ChangeEndianess(TimestampBig);
                    SSRCBig = ChangeEndianess(SSRCBig);
                }
                    
                char *BigC = (char *)&SeqBig;
                for (char i = 0; i < sizeof(uint16_t); i++)
                    Data[i + 2] = BigC[i];

                BigC = (char *)&TimestampBig;
                for (char i = 0; i < sizeof(int); i++)
                    Data[i + 4] = BigC[i];

                BigC = (char *)&SSRCBig;
                for (char i = 0; i < sizeof(int); i++)
                    Data[i + 8] = BigC[i];

                char Nonce[NonceSize];
                memcpy(Nonce, &Data[0], RTPHeaderSize);
                memset(Nonce + RTPHeaderSize, 0, RTPHeaderSize);

                Timestamp += Ret;

                crypto_secretbox_easy((uint8_t*)Data.data() + RTPHeaderSize, OpusBuf, OpusSize, (uint8_t*)Nonce, m_SecKey.data());
                
                // if(Wait > 0 && Wait <= LastSpeachTime)
                //     std::this_thread::sleep_for(std::chrono::milliseconds(Wait));

                m_UDPSocket.sendto(Data);

                LastSpeachTime = (float)(Ret * 2) / (FREQUENCY * CHANNEL) * 1000.f;

                // //Speach time.
                int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                int64_t TimeDiff = (now - LastSendTime);
                int Wait = LastSpeachTime - TimeDiff;

                Wait = std::min(Wait, (int)LastSpeachTime);

                // llog << linfo << "Wait: " << (float)Wait << " TimeDiff: " << (float)TimeDiff << " LastSpeachTime: " << LastSpeachTime << lendl;

                std::this_thread::sleep_for(std::chrono::milliseconds(Wait));

                LastSendTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); //std::chrono:://clock();
                
                // std::this_thread::sleep_for(std::chrono::milliseconds((int)LastSpeachTime));
            }
            else
            {
                llog << lerror << "Error during encoding opus data." << lendl;
                break;
            }    

            if(Ret < (Size / 2))
            {
                llog << linfo << "Finish playing." << lendl;
                break;
            }
        }

        delete[] Buf;
        delete[] OpusBuf;

        opus_encoder_destroy(Encoder);
        SetSpeaking(false);

        m_Callback(m_GuildID);
        m_Source = nullptr;
    }

    /**
     * @brief Builds and sends a payload object.
     */
    void CVoiceSocket::SendOP(OPCodes OP, const std::string &D)
    {
        SPayload Pay;
        Pay.OP = (uint32_t)OP;
        Pay.D = D;

        try
        {
            CJSON json;
            m_Socket.send(json.Serialize(Pay));
        }
        catch(const CJSONException& e)
        {
            llog << lerror << "Failed to serialize the Payload object. Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
        }
    }

    void CVoiceSocket::OnWebsocketEvent(const ix::WebSocketMessagePtr& msg)
    {
        switch (msg->type)
        {
            case ix::WebSocketMessageType::Error:
            {
                llog << lerror << "Websocket error " << msg->errorInfo.reason << lendl;
            }break;

            case ix::WebSocketMessageType::Close:
            {
                m_Terminate = true;
                llog << linfo << "Websocket closed code " <<  msg->closeInfo.code << " Reason " <<  msg->closeInfo.reason << lendl;
            }break;
        
            case ix::WebSocketMessageType::Message:
            {   
                CJSON json;
                SPayload Pay;

                try
                {
                    Pay = json.Deserialize<SPayload>(msg->str);
                }
                catch(const CJSONException& e)
                {
                    llog << lerror << "Failed to parse JSON Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
                    return;
                }
                
                switch ((OPCodes)Pay.OP)
                {
                    case OPCodes::SESSION_DESCIPTION:
                    {
                        json.ParseObject(Pay.D);
                        m_SecKey = json.GetValue<std::vector<uint8_t>>("secret_key");

                        if(m_Source)
                            StartSpeaking(m_Source);

                        llog << linfo << "Voice channel connected" << lendl;
                    }break;

                    case OPCodes::READY:
                    {
                        try
                        {
                            json.ParseObject(Pay.D);

                            m_SSRC = json.GetValue<int>("ssrc");

                            std::string errmsg;
                            if(!m_UDPSocket.init(json.GetValue<std::string>("ip"), json.GetValue<int>("port"), errmsg))
                                llog << lerror << "Failed to create socket. " << errmsg << lendl;
                            else
                            {
                                uint8_t Packet[74] = {0};
                                Packet[1] = 0x1;    //Type
                                Packet[3] = 70;     //Length field

                                int SSRCBig = m_SSRC;

                                if(IsLittleEndian())
                                    SSRCBig = ChangeEndianess(SSRCBig);


                                char *SSRC = (char *)&SSRCBig;
                                for (char i = 0; i < sizeof(int); i++)
                                    Packet[i + 4] = SSRC[i];

                                m_UDPSocket.sendto(std::string((char*)Packet, sizeof(Packet)));  
                                std::thread([this]() mutable
                                {
                                    std::vector<uint8_t> Data(sizeof(Packet));

                                    while (true)
                                    {
                                        ssize_t Ret = m_UDPSocket.recvfrom(&Data[0], Data.size());
                                        if(Ret > 0)
                                        {
                                            std::string IP; 
                                            for (size_t i = 8; i < Data.size(); i++)
                                            {
                                                if(!Data[i])
                                                    break;

                                                IP += Data[i];
                                            }

                                            short Port;
                                            char Shift = 8;

                                            for (size_t i = 72; i < Data.size(); i++)
                                            {
                                                Port = (Data[i] << Shift);
                                                Shift -= Shift;
                                            }

                                            CJSON json;
                                            json.AddPair("address", IP);
                                            json.AddPair("port", Port);
                                            json.AddPair("mode", std::string("xsalsa20_poly1305"));

                                            std::string JData = json.Serialize();

                                            json.AddPair("protocol", std::string("udp"));
                                            json.AddJSON("data", JData);

                                            SendOP(OPCodes::SELECT_PROTOCOL, json.Serialize());
                                            break;
                                        }
                                        else if(Ret < 0 && m_UDPSocket.isWaitNeeded())
                                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                        else if(Ret < 0)
                                            break;
                                    }
                                }).detach();          
                            }
                        }
                        catch(const CJSONException& e)
                        {
                            llog << lerror << "Failed to parse JSON Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
                            return;
                        }
                    }break;

                    case OPCodes::RESUMED:
                    {
                        llog << linfo << "Voice resumed" << lendl;
                    }break;

                    case OPCodes::HELLO:
                    {
                        try
                        {
                            json.ParseObject(Pay.D);
                            m_HeartbeatInterval = json.GetValue<uint32_t>("heartbeat_interval");
                        }
                        catch(const CJSONException& e)
                        {
                            llog << lerror << "Failed to parse JSON Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
                            return;
                        }

                        CJSON id;
                        id.AddPair("server_id", m_GuildID);
                        id.AddPair("session_id", m_SessionID);
                        id.AddPair("token", m_Token);

                        if(!m_Reconnect)
                        {
                            id.AddPair("user_id", m_ClientID);
                            SendOP(OPCodes::IDENTIFY, id.Serialize());
                        }
                        else
                        {
                            m_Reconnect = false;
                            SendOP(OPCodes::RESUME, id.Serialize());
                        }

                        m_HeartACKReceived = true;
                        m_Terminate = false;

                        if(m_Heartbeat.joinable())
                            m_Heartbeat.join();

                        m_Heartbeat = std::thread(&CVoiceSocket::Heartbeat, this);
                    }break;

                    case OPCodes::HEARTBEAT_ACK:
                    {
                        m_HeartACKReceived = true;
                    }break;
                }
            }break;
        }
    }

    /**
     * @brief Sends a heartbeat.
     */
    void CVoiceSocket::Heartbeat()
    {
        while (!m_Terminate)
        {
            //Start a reconnect.
            if(!m_HeartACKReceived)
            {
                m_Reconnect = true;
                m_Socket.close();
                m_Socket.start();
                m_Terminate = true;
                break;
            }

            SendOP(OPCodes::HEARTBEAT, "5");
            m_HeartACKReceived = false;

            std::this_thread::sleep_for(std::chrono::milliseconds(m_HeartbeatInterval));
        }
    }

    CVoiceSocket::~CVoiceSocket()
    {

    }
} // namespace DiscordBot
