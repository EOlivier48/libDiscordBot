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

#include <controller/IController.hpp>
#include <IDiscordClient.hpp>
#include "../commands/HelpCommand.hpp"
#include "JSONCmdsConfig.hpp"

namespace DiscordBot
{
    //Trims a string.
    inline std::string trim(std::string str)
    {
        size_t Pos = str.find_first_not_of("\t\n\v\f\r ");
        if(Pos != std::string::npos)
            str = str.erase(0, Pos);

        Pos = str.find_last_not_of("\t\n\v\f\r ");
        if(Pos != std::string::npos)
            str = str.erase(Pos + 1);

        return str;
    }

    IController::IController(IDiscordClient *client) : Client(client), Prefix("!")
    {
        //Default config
        CmdsConfig = CommandsConfig(new CJSONCmdsConfig());

        RegisterCommand<CHelpCommand>({"h", "Prints a help dialog", 0, ""}, this, Client);
        RegisterCommand<CHelpCommand>({"help", "Prints a help dialog", 0, ""}, this, Client);
    }

    /**
     * @brief Called if a new message was sended. Process the message and call associated commands.
     * 
     * @param msg: Message object. See @see ::CMessage for more informations.
     */
    void IController::OnMessage(Message msg)
    {
        bool Handled = false;
        OnMessage(msg, Handled);

        if(!Handled)
        {
            //Checks if the command start with the prefix.
            if(msg->Content.compare(0, Prefix.size(), Prefix) == 0)
            {
                size_t Pos = msg->Content.find_first_of(' ', Prefix.size());
                std::string Cmd = msg->Content.substr(Prefix.size(), Pos - Prefix.size());

                auto IT = m_CommandDescs.find(Cmd);
                if(IT != m_CommandDescs.end())
                {
                    CommandContext ctx = CommandContext(new CCommandContext());
                    ctx->Msg = msg;
                    ctx->Command = Cmd;

                    if(Pos != std::string::npos)
                    {
                        std::string Params = trim(msg->Content.substr(Pos));
                        if(IT->second.ParamDelimiter.empty())
                            ctx->Params.push_back(Params);
                        else
                        {
                            Pos = 0;
                            size_t Beg = 0;
                            while (true)                        
                            {
                                Pos = Params.find(IT->second.ParamDelimiter, Pos);
                                ctx->Params.push_back(trim(Params.substr(Beg, Pos)));

                                if(Pos == std::string::npos)
                                    break;

                                Beg = Pos = Params.find_first_not_of(IT->second.ParamDelimiter, Pos + 1);
                            }   
                        }
                    }

                    //Executes the command.
                    if(ctx->Params.size() == IT->second.ParamCount || IT->second.ParamCount == -1)
                    {
                        auto Command = m_Commands[Cmd]->Create();
                        Command->OnExecute(ctx);
                    }
                }
            }
        }
    }
} // namespace DiscordBot
