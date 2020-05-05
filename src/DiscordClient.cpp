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

#include "DiscordClient.hpp"
#include <ixwebsocket/IXHttpClient.h>
#include <iostream>

#define CLOG_IMPLEMENTATION
#include <Log.hpp>

namespace DiscordBot
{
    /**
     * @param Token: Your Discord bot token. Which you have created <a href="https://discordapp.com/developers/applications">here</a>.
     * 
     * @return Returns a new DiscordClient object.
     */
    DiscordClient IDiscordClient::Create(const std::string &Token)
    {
        return DiscordClient(new CDiscordClient(Token));
    }

    /**
     * @brief Const adler32 implementation to use strings in switch cases.
     */
    const static int BASE = 65521;
    constexpr size_t Adler32(const char *Data)
    {
        size_t S1 = 1 & 0xFFFF;
        size_t S2 = (1 >> 16) & 0xFFFF;

        const char *Beg = Data;
        while (*Beg)
        {
            S1 = (S1 + *Beg) % BASE;
            S2 = (S2 + S1) % BASE;
            Beg++;
        }

        return (S2 << 16) + S1;
    }

    /**
     * @brief Joins a audio channel.
     * 
     * @param channel: The voice channel to join.
     */
    void CDiscordClient::Join(Channel channel)
    {
        if (!channel || channel->GuildID.empty() || channel->ID.empty())
            return;

        CJSON json;
        json.AddPair("guild_id", channel->GuildID);
        json.AddPair("channel_id", channel->ID);
        json.AddPair("self_mute", false);
        json.AddPair("self_deaf", false);

        SendOP(OPCodes::VOICE_STATE_UPDATE, json.Serialize());
    }

    /**
     * @brief Leaves the audio channel.
     * 
     * @param channel: The voice channel to leave.
     */
    void CDiscordClient::Leave(Channel channel)
    {
        if (!channel || channel->GuildID.empty() || channel->ID.empty())
            return;

        CJSON json;
        json.AddPair("guild_id", channel->GuildID);
        json.AddPair("channel_id", nullptr);
        json.AddPair("self_mute", false);
        json.AddPair("self_deaf", false);

        SendOP(OPCodes::VOICE_STATE_UPDATE, json.Serialize());
    }

    /**
     * @brief Sends a message to a given channel.
     * 
     * @param channel: Text channel which will receive the message.
     * @param Text: Text to send;
     * @param TTS: True to enable tts.
     */
    void CDiscordClient::SendMessage(Channel channel, const std::string Text, bool TTS)
    {
        if(channel->Type != ChannelTypes::GUILD_TEXT)
            return;

        ix::HttpClient http;
        ix::HttpRequestArgsPtr args = ix::HttpRequestArgsPtr(new ix::HttpRequestArgs());

        //Add the bot token.
        args->extraHeaders["Authorization"] = "Bot " + m_Token;
        args->extraHeaders["Content-Type"] = "application/json";

        CJSON json;
        json.AddPair("content", Text);
        json.AddPair("tts", TTS);

        auto res = http.post(std::string(BASE_URL) + "/channels/" + channel->ID + "/messages", json.Serialize(), args);
        if (res->statusCode != 200)
            llog << lerror << "Failed to send message HTTP: " << res->statusCode << " MSG: " << res->errorMsg << lendl;
    }

    /**
     * @brief Runs the bot. The call returns if you calls @see Quit().
     */
    void CDiscordClient::Run()
    {
        //Needed for windows.
        ix::initNetSystem();

        ix::HttpClient http;
        ix::HttpRequestArgsPtr args = ix::HttpRequestArgsPtr(new ix::HttpRequestArgs());

        //Add the bot token.
        args->extraHeaders["Authorization"] = "Bot " + m_Token;

        //Requests the gateway endpoint for bots.
        auto res = http.get(BASE_URL + std::string("/gateway/bot"), args);
        if (res->statusCode == 200)
        {
            try
            {
                CJSON json;
                m_Gateway = json.Deserialize<std::shared_ptr<SGateway>>(res->payload);
            }
            catch (const CJSONException &e)
            {
                llog << lerror << "Failed to parse JSON Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
                return;
            }

            //Connects to discords websocket.
            m_Socket.setUrl(m_Gateway->URL + "/?v=6&encoding=json");
            m_Socket.setOnMessageCallback(std::bind(&CDiscordClient::OnWebsocketEvent, this, std::placeholders::_1));
            m_Socket.start();

            //Runs until the bot quits.
            while (!m_Quit)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        else
            llog << lerror << "HTTP " << res->statusCode << " Error " << res->errorMsg << lendl;
    }

    /**
     * @brief Quits the bot. And disconnects all voice states.
     */
    void CDiscordClient::Quit()
    {
        m_Socket.close();
        m_Terminate = true;

        if (m_Heartbeat.joinable())
            m_Heartbeat.join();

        m_Quit = true;

        if (m_Controller)
        {
            m_Controller->OnDisconnect();
            m_Controller->OnQuit();
            m_Controller = nullptr;
        }
    }

    /**
     * @brief Connects to the given channel and uses the source to speak.
     * 
     * @param channel: The voice channel to connect to.
     * @param source: The audio source for speaking.
     * 
     * @return Returns true if the connection succeeded.
     */
    bool CDiscordClient::StartSpeaking(Channel channel, AudioSource source)
    {
        if (!channel || channel->GuildID.empty())
            return false;

        VoiceSockets::iterator IT = m_VoiceSockets.find(channel->GuildID);
        if (IT != m_VoiceSockets.end())
        {
            IT->second->StartSpeaking(source);
            return true;
        }
        else
        {
            m_AudioSources[channel->GuildID] = source;
            return true;
        }

        return false;
    }

    /**
     * @brief Receives all websocket events from discord. This is the heart of the bot.
     */
    void CDiscordClient::OnWebsocketEvent(const ix::WebSocketMessagePtr &msg)
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
                llog << linfo << "Websocket closed code " << msg->closeInfo.code << " Reason " << msg->closeInfo.reason << lendl;
            }break;

            case ix::WebSocketMessageType::Message:
            {
                CJSON json;
                SPayload Pay;

                try
                {
                    Pay = json.Deserialize<SPayload>(msg->str);
                }
                catch (const CJSONException &e)
                {
                    llog << lerror << "Failed to parse JSON Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
                    return;
                }

                switch ((OPCodes)Pay.OP)
                {
                    case OPCodes::DISPATCH:
                    {
                        m_LastSeqNum = Pay.S;
                        std::hash<std::string> hash;

                        //Gateway Events https://discordapp.com/developers/docs/topics/gateway#commands-and-events-gateway-events
                        switch (Adler32(Pay.T.c_str()))
                        {
                        case Adler32("READY"):
                        {
                            json.ParseObject(Pay.D);
                            m_SessionID = json.GetValue<std::string>("session_id");

                            json.ParseObject(json.GetValue<std::string>("user"));
                            m_BotUser = CreateUser(json);

                            llog << linfo << "Connected with Discord! " << m_Socket.getUrl() << lendl;

                            if (m_Controller)
                                m_Controller->OnReady();
                        }
                        break;

                        case Adler32("GUILD_CREATE"):
                        {
                            json.ParseObject(Pay.D);

                            Guild guild = Guild(new CGuild());
                            guild->ID = json.GetValue<std::string>("id");
                            guild->Name = json.GetValue<std::string>("name");

                            //Get all Channels;
                            std::vector<std::string> Array = json.GetValue<std::vector<std::string>>("channels");
                            for (auto &&e : Array)
                            {
                                CJSON jChannel;
                                jChannel.ParseObject(e);

                                Channel Tmp = CreateChannel(jChannel);
                                Tmp->GuildID = guild->ID;
                                guild->Channels[Tmp->ID] = Tmp;
                            }

                            //Get all members.
                            Array = json.GetValue<std::vector<std::string>>("members");
                            for (auto &&e : Array)
                            {
                                CJSON Member;
                                Member.ParseObject(e);

                                GuildMember Tmp = CreateMember(Member);

                                if (Tmp->UserRef)
                                    guild->Members[Tmp->UserRef->ID] = Tmp;
                            }

                            //Get all voice states.
                            Array = json.GetValue<std::vector<std::string>>("voice_states");
                            for (auto &&e : Array)
                            {
                                CJSON State;
                                State.ParseObject(e);

                                CreateVoiceState(State, guild);
                            }

                            m_Guilds[guild->ID] = guild;
                        }break;

                        case Adler32("GUILD_DELETE"):
                        {
                            json.ParseObject(Pay.D);
                            auto IT = m_VoiceSockets.find(json.GetValue<std::string>("id"));
                            if(IT != m_VoiceSockets.end())
                                m_VoiceSockets.erase(IT);

                            auto GIT = m_Guilds.find(json.GetValue<std::string>("id"));
                            if(GIT != m_Guilds.end())
                                m_Guilds.erase(GIT);

                            llog << linfo << "GUILD_DELETE" << lendl;
                        }break;

                        case Adler32("VOICE_STATE_UPDATE"):
                        {
                            json.ParseObject(Pay.D);
                            VoiceState Tmp = CreateVoiceState(json, nullptr);

                            if (m_Controller && Tmp->GuildRef)
                            {
                                if(Tmp->UserRef)
                                {
                                    if(Tmp->UserRef->ID == m_BotUser->ID && !Tmp->ChannelRef)
                                    {
                                        auto IT = m_VoiceSockets.find(Tmp->GuildRef->ID);
                                        if(IT != m_VoiceSockets.end())
                                            m_VoiceSockets.erase(IT);
                                    }

                                    auto IT = Tmp->GuildRef->Members.find(Tmp->UserRef->ID);
                                    if(IT != Tmp->GuildRef->Members.end())
                                        m_Controller->OnVoiceStateUpdate(IT->second);
                                }
                            }   
                        }break;

                        case Adler32("VOICE_SERVER_UPDATE"):
                        {
                            json.ParseObject(Pay.D);
                            Guilds::iterator GIT = m_Guilds.find(json.GetValue<std::string>("guild_id"));
                            if (GIT != m_Guilds.end())
                            {
                                auto UIT = GIT->second->Members.find(m_BotUser->ID);
                                if (UIT != GIT->second->Members.end())
                                {
                                    VoiceSocket Socket = VoiceSocket(new CVoiceSocket(json, UIT->second->State->SessionID, m_BotUser->ID));
                                    Socket->SetOnSpeakFinish(std::bind(&CDiscordClient::OnSpeakFinish, this, std::placeholders::_1));
                                    m_VoiceSockets[GIT->second->ID] = Socket;

                                    AudioSources::iterator IT = m_AudioSources.find(GIT->second->ID);
                                    if (IT != m_AudioSources.end())
                                    {
                                        Socket->StartSpeaking(IT->second);
                                        m_AudioSources.erase(IT);
                                    }
                                }
                            }
                        }break;

                        case Adler32("MESSAGE_CREATE"):
                        {
                            json.ParseObject(Pay.D);
                            Message msg = CreateMessage(json);

                            if (m_Controller)
                                m_Controller->OnMessage(msg);
                        }break;

                        case Adler32("RESUMED"):
                        {
                            llog << linfo << "Resumed" << lendl;

                            if (m_Controller)
                                m_Controller->OnResume();
                        } break;

                        case Adler32("INVALID_SESSION"):
                        {
                            if (Pay.D == "true")
                                SendResume();
                            else
                                Quit();

                            llog << linfo << "INVALID_SESSION" << lendl;
                        }break;
                    }
                }break;

                case OPCodes::HELLO:
                {
                    try
                    {
                        json.ParseObject(Pay.D);
                        m_HeartbeatInterval = json.GetValue<uint32_t>("heartbeat_interval");
                    }
                    catch (const CJSONException &e)
                    {
                        llog << lerror << "Failed to parse JSON Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
                        return;
                    }

                    if (m_SessionID.empty())
                        SendIdentity();
                    else
                        SendResume();

                    m_HeartACKReceived = true;
                    m_Terminate = false;

                    if (m_Heartbeat.joinable())
                        m_Heartbeat.join();

                    m_Heartbeat = std::thread(&CDiscordClient::Heartbeat, this);
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
    void CDiscordClient::Heartbeat()
    {
        while (!m_Terminate)
        {
            //Start a reconnect.
            if (!m_HeartACKReceived)
            {
                m_Socket.close();

                m_Users.clear();
                m_Guilds.clear();
                m_VoiceSockets.clear();

                if (m_Controller)
                    m_Controller->OnDisconnect();

                m_Socket.start();
                m_Terminate = true;
                break;
            }

            SendOP(OPCodes::HEARTBEAT, m_LastSeqNum != -1 ? std::to_string(m_LastSeqNum) : "");
            m_HeartACKReceived = false;

            std::this_thread::sleep_for(std::chrono::milliseconds(m_HeartbeatInterval));
        }
    }

    /**
     * @brief Builds and sends a payload object.
     */
    void CDiscordClient::SendOP(CDiscordClient::OPCodes OP, const std::string &D)
    {
        SPayload Pay;
        Pay.OP = (uint32_t)OP;
        Pay.D = D;

        try
        {
            CJSON json;
            m_Socket.send(json.Serialize(Pay));
        }
        catch (const CJSONException &e)
        {
            llog << lerror << "Failed to serialize the Payload object. Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
        }
    }

    /**
     * @brief Sends the identity.
     */
    void CDiscordClient::SendIdentity()
    {
        SIdentify id;
        id.Token = m_Token;
        id.Properties["$os"] = "linux";
        id.Properties["$browser"] = "linux";
        id.Properties["$device"] = "linux";
        id.Intents = Intent::GUILDS | Intent::GUILD_VOICE_STATES | Intent::GUILD_MESSAGES | Intent::DIRECT_MESSAGES;

        CJSON json;
        SendOP(OPCodes::IDENTIFY, json.Serialize(id));
    }

    /**
     * @brief Sends a resume request.
     */
    void CDiscordClient::SendResume()
    {
        SResume resume;
        resume.Token = m_Token;
        resume.SessionID = m_SessionID;
        resume.Seq = m_LastSeqNum;

        CJSON json;
        SendOP(OPCodes::RESUME, json.Serialize(resume));
    }

    /**
     * @brief Called from voice socket if a audio source finished.
     */
    void CDiscordClient::OnSpeakFinish(const std::string &Guild)
    {
        if(m_Controller)
        {
            auto IT = m_Guilds.find(Guild);
            if(IT != m_Guilds.end())
                m_Controller->OnEndSpeaking(IT->second);
        }
    }

    User CDiscordClient::CreateUser(CJSON &json)
    {
        User Ret = User(new CUser());

        Ret->ID = json.GetValue<std::string>("id");
        Ret->Username = json.GetValue<std::string>("username");
        Ret->Discriminator = json.GetValue<std::string>("discriminator");
        Ret->Avatar = json.GetValue<std::string>("avatar");
        Ret->Bot = json.GetValue<bool>("bot");
        Ret->System = json.GetValue<bool>("system");
        Ret->MFAEnabled = json.GetValue<bool>("mfa_enabled");
        Ret->Locale = json.GetValue<std::string>("locale");
        Ret->Verified = json.GetValue<bool>("verified");
        Ret->Email = json.GetValue<std::string>("email");
        Ret->Flags = (UserFlags)json.GetValue<int>("flags");
        Ret->PremiumType = (PremiumTypes)json.GetValue<int>("premium_type");
        Ret->PublicFlags = (UserFlags)json.GetValue<int>("public_flags");

        m_Users[Ret->ID] = Ret;

        return Ret;
    }

    GuildMember CDiscordClient::CreateMember(CJSON &json)
    {
        GuildMember Ret = GuildMember(new CGuildMember());
        std::string UserInfo = json.GetValue<std::string>("user");
        User member;

        //Gets the user which is associated with the member.
        if (!UserInfo.empty())
        {
            CJSON user;
            user.ParseObject(UserInfo);

            Users::iterator IT = m_Users.find(user.GetValue<std::string>("id"));
            if (IT != m_Users.end())
                member = IT->second;
            else
                member = CreateUser(user);
        }

        Ret->UserRef = member;
        Ret->Nick = json.GetValue<std::string>("nick");
        Ret->Roles = json.GetValue<std::vector<std::string>>("roles");
        Ret->JoinedAt = json.GetValue<std::string>("joined_at");
        Ret->PremiumSince = json.GetValue<std::string>("premium_since");
        Ret->Deaf = json.GetValue<bool>("deaf");
        Ret->Mute = json.GetValue<bool>("mute");

        return Ret;
    }

    VoiceState CDiscordClient::CreateVoiceState(CJSON &json, Guild guild)
    {
        VoiceState Ret = VoiceState(new CVoiceState());

        if (!guild)
        {
            Guilds::iterator IT = m_Guilds.find(json.GetValue<std::string>("guild_id"));
            if (IT != m_Guilds.end())
                Ret->GuildRef = IT->second;
        }
        else
            Ret->GuildRef = guild;

        auto IT = m_Users.find(json.GetValue<std::string>("user_id"));
        if (IT != m_Users.end())
            Ret->UserRef = IT->second;

        if (Ret->GuildRef)
        {
            auto CIT = Ret->GuildRef->Channels.find(json.GetValue<std::string>("channel_id"));
            if (CIT != Ret->GuildRef->Channels.end())
                Ret->ChannelRef = CIT->second;

            GuildMember Member;

            //Adds this voice state to the guild member.
            auto MIT = Ret->GuildRef->Members.find(json.GetValue<std::string>("user_id"));
            if (MIT != Ret->GuildRef->Members.end())
                Member = MIT->second;
            else
            {
                //Creates a new member.
                try
                {
                    CJSON JMember;
                    JMember.ParseObject(json.GetValue<std::string>("member"));

                    Member = CreateMember(JMember);
                    if (Member->UserRef)
                        Ret->GuildRef->Members[Member->UserRef->ID] = Member;
                }
                catch (const CJSONException &e)
                {
                    llog << lerror << "Failed to parse JSON for VoiceState member Enumtype: " << GetEnumName(e.GetErrType()) << " what(): " << e.what() << lendl;
                    return Ret;
                }
            }

            //Removes the voice state if the user isn't in a voice channel.
            if (!Ret->ChannelRef && Member)
            {
                Member->State = nullptr;
                return Ret;
            }
            else if(Member)
                Member->State = Ret;
        }

        Ret->SessionID = json.GetValue<std::string>("session_id");
        Ret->Deaf = json.GetValue<bool>("deaf");
        Ret->Mute = json.GetValue<bool>("mute");
        Ret->SelfDeaf = json.GetValue<bool>("self_deaf");
        Ret->SelfMute = json.GetValue<bool>("self_mute");
        Ret->SelfStream = json.GetValue<bool>("self_stream");
        Ret->Supress = json.GetValue<bool>("suppress");

        return Ret;
    }

    Channel CDiscordClient::CreateChannel(CJSON &json)
    {
        Channel Ret = Channel(new CChannel());

        Ret->ID = json.GetValue<std::string>("id");
        Ret->Type = (ChannelTypes)json.GetValue<int>("type");
        Ret->GuildID = json.GetValue<std::string>("guild_id");
        Ret->Position = json.GetValue<int>("position");

        std::vector<std::string> Array = json.GetValue<std::vector<std::string>>("permission_overwrites");
        for (auto &&e : Array)
        {
            PermissionOverwrites ov = PermissionOverwrites(new CPermissionOverwrites());
            CJSON jov;
            jov.ParseObject(e);

            ov->ID = jov.GetValue<std::string>("id");
            ov->Type = jov.GetValue<std::string>("type");
            ov->Allow = jov.GetValue<int>("allow");
            ov->Deny = jov.GetValue<int>("deny");

            Ret->Overwrites.push_back(ov);
        }

        Ret->Name = json.GetValue<std::string>("name");
        Ret->Topic = json.GetValue<std::string>("topic");
        Ret->NSFW = json.GetValue<bool>("nsfw");
        Ret->LastMessageID = json.GetValue<std::string>("last_message_id");
        Ret->Bitrate = json.GetValue<int>("bitrate");
        Ret->UserLimit = json.GetValue<int>("user_limit");
        Ret->RateLimit = json.GetValue<int>("rate_limit_per_user");

        Array = json.GetValue<std::vector<std::string>>("recipients");
        for (auto &&e : Array)
        {
            CJSON juser;
            juser.ParseObject(e);

            Users::iterator IT = m_Users.find(juser.GetValue<std::string>("id"));
            User user;

            if (IT != m_Users.end())
                user = IT->second;
            else
                user = CreateUser(juser);

            Ret->Recipients.push_back(user);
        }

        Ret->Icon = json.GetValue<std::string>("icon");
        Ret->OwnerID = json.GetValue<std::string>("owner_id");
        Ret->AppID = json.GetValue<std::string>("application_id");
        Ret->ParentID = json.GetValue<std::string>("parent_id");
        Ret->LastPinTimestamp = json.GetValue<std::string>("last_pin_timestamp");

        return Ret;
    }

    Message CDiscordClient::CreateMessage(CJSON &json)
    {
        Message Ret = Message(new CMessage());
        Channel channel;

        Guilds::iterator IT = m_Guilds.find(json.GetValue<std::string>("guild_id"));
        if (IT != m_Guilds.end())
        {
            Ret->GuildRef = IT->second;
            std::map<std::string, Channel>::iterator CIT = Ret->GuildRef->Channels.find(json.GetValue<std::string>("channel_id"));
            if (CIT != Ret->GuildRef->Channels.end())
                channel = CIT->second;
        }

        //Creates a dummy object for DMs.
        if (!channel)
        {
            channel = Channel(new CChannel());
            channel->ID = json.GetValue<std::string>("channel_id");
        }

        Ret->ID = json.GetValue<std::string>("id");
        Ret->ChannelRef = channel;

        std::string UserJson = json.GetValue<std::string>("author");
        if (!UserJson.empty())
        {
            CJSON juser;
            juser.ParseObject(UserJson);

            User user;
            Users::iterator UIT = m_Users.find(juser.GetValue<std::string>("id"));
            if (UIT != m_Users.end())
                user = UIT->second;
            else
                user = CreateUser(juser);

            Ret->Author = user;

            //Gets the guild member, if this message is not a dm.
            if (Ret->GuildRef)
            {
                auto MIT = Ret->GuildRef->Members.find(Ret->Author->ID);
                if (MIT != Ret->GuildRef->Members.end())
                    Ret->Member = MIT->second;
            }
        }

        Ret->Content = json.GetValue<std::string>("content");
        Ret->Timestamp = json.GetValue<std::string>("timestamp");
        Ret->EditedTimestamp = json.GetValue<std::string>("edited_timestamp");
        Ret->Mention = json.GetValue<bool>("mention_everyone");

        std::vector<std::string> Array = json.GetValue<std::vector<std::string>>("mentions");
        for (auto &&e : Array)
        {
            CJSON jmention;
            jmention.ParseObject(e);

            User user;
            Users::iterator UIT = m_Users.find(jmention.GetValue<std::string>("id"));
            if (UIT != m_Users.end())
                user = UIT->second;
            else
                user = CreateUser(jmention);

            bool Found = false;

            if (Ret->GuildRef)
            {
                auto MIT = Ret->GuildRef->Members.find(Ret->Author->ID);
                if (MIT != Ret->GuildRef->Members.end())
                {
                    Found = true;
                    Ret->Mentions.push_back(MIT->second);
                }
            }

            //Create a fake Guildmember for DMs.
            if (!Found)
            {
                Ret->Mentions.push_back(GuildMember(new CGuildMember()));
                Ret->Mentions.back()->UserRef = user;
            }
        }

        return Ret;
    }
} // namespace DiscordBot
