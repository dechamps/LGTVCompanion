// See LGTV Companion UI.cpp for additional details
#include "Service.h"

using namespace std;
namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
using json = nlohmann::json;

mutex mMutex;

CSession::CSession(SESSIONPARAMETERS* Params)
{
	Parameters = *Params; 
}
CSession::~CSession()
{
    
}
void CSession::Run()
{
    //thread safe section
    while (!mMutex.try_lock())
        Sleep(MUTEX_WAIT);
    Parameters.Enabled = true;
    mMutex.unlock();
}
void CSession::Stop()
{
    //thread safe section
    while (!mMutex.try_lock())
        Sleep(MUTEX_WAIT);
    Parameters.Enabled = false;
    mMutex.unlock();
}
SESSIONPARAMETERS CSession::GetParams(void)
{
    SESSIONPARAMETERS copy;
    //thread safe section
    while (!mMutex.try_lock())
        Sleep(MUTEX_WAIT);
    copy = Parameters;
    mMutex.unlock();
    return copy;
}
void CSession::TurnOnDisplay(void)
{
    string s;

    //thread safe section
    while (!mMutex.try_lock())
        Sleep(MUTEX_WAIT);

    //discard the event if thread is running or the event is too close to the previous
    if (!ThreadedOperationsRunning && (time(0) - TimeStamp > THREAD_WAIT))
    {
        s = "Creating thread for "; s += Parameters.DeviceId; s += ": DisplayPowerOnThread";
        
        ThreadedOperationsRunning = true;
        TimeStamp = time(0);
        thread thread_obj(DisplayPowerOnThread, &Parameters, &ThreadedOperationsRunning, Parameters.PowerOnTimeout);
        thread_obj.detach();
    }
    else
    {
        s = "Thread omitted for "; s += Parameters.DeviceId; s += ": DisplayPowerOnThread";
    }
    mMutex.unlock();
    Log(s);
    return;
}
void CSession::TurnOffDisplay(void)
{
    string s;

    //thread safe section
    while (!mMutex.try_lock())
        Sleep(MUTEX_WAIT);
    
    //discard the event if thread is running or the event is too close to the previous
    if (!ThreadedOperationsRunning && (time(0) - TimeStamp > THREAD_WAIT) && Parameters.SessionKey !="")
    {
        s = "Creating thread for "; s += Parameters.DeviceId; s += ": DisplayPowerOffThread";

        ThreadedOperationsRunning = true;
        TimeStamp = time(0);
        thread thread_obj(DisplayPowerOffThread, &Parameters, &ThreadedOperationsRunning);
        thread_obj.detach();
    }
    else
    {
        s = "Thread omitted for "; s += Parameters.DeviceId; s += ": DisplayPowerOffThread";
        if (Parameters.SessionKey == "")
            s += " (no session key)";
    }

    mMutex.unlock();
    Log(s);
    return;
}
//   Called when a display power event (on/off) has occured which the device should respond to
void CSession::SystemEvent(DWORD dwMsg)
{
    if (dwMsg == SYSTEM_EVENT_FORCEON)
        TurnOnDisplay();
    if (dwMsg == SYSTEM_EVENT_FORCEOFF)
        TurnOffDisplay();
    
    //thread safe section
    while (!mMutex.try_lock())
        Sleep(MUTEX_WAIT);
    if (!Parameters.Enabled)
    {
        mMutex.unlock();
        return;
    }
    mMutex.unlock();


    if (Parameters.AwayAuto && dwMsg == SYSTEM_EVENT_DISPLAYON)
        if(!SuspendDisplayPowerEvents || time(0)-TimeStamp > 30 )
            TurnOnDisplay();
    if (Parameters.AwayAuto && dwMsg == SYSTEM_EVENT_DISPLAYOFF)
        if(!SuspendDisplayPowerEvents || time(0) - TimeStamp > 30)
            TurnOffDisplay();
    if (Parameters.PowerAuto && dwMsg == SYSTEM_EVENT_RESUME)
    {
        SuspendDisplayPowerEvents = false;
        TurnOnDisplay();
    }
   if (Parameters.PowerAuto && dwMsg == SYSTEM_EVENT_RESUMEAUTO) 
       SuspendDisplayPowerEvents = false;

//    if (Parameters.PowerAuto && dwMsg == SYSTEM_EVENT_REBOOT)
//        TurnOffDisplay();

    if (Parameters.PowerAuto && dwMsg == SYSTEM_EVENT_SHUTDOWN)
    {
        SuspendDisplayPowerEvents = true;
        TurnOffDisplay();
    }
    if (Parameters.PowerAuto && dwMsg == SYSTEM_EVENT_SUSPEND)
    {
        SuspendDisplayPowerEvents = true;
        TurnOffDisplay();
    }
    if (Parameters.PowerAuto && dwMsg == SYSTEM_EVENT_UNSURE)
    {
        SuspendDisplayPowerEvents = true;
        TurnOffDisplay();
    }      

    
}
//   THREAD: Spawned when the device should power ON. This thread manages the pairing key from the display and verifies that the display has been powered on
void DisplayPowerOnThread(SESSIONPARAMETERS * CallingSessionParameters, bool * CallingSessionThreadRunning, int Timeout)
{
    if (!CallingSessionParameters)
        return;

    string hostip = CallingSessionParameters->IP;
    string key = CallingSessionParameters->SessionKey;
    string ck = "CLIENTKEYx0x0x0";
    string handshake; 
    time_t origtim = time(0);
 
    thread wolthread(WOLthread, CallingSessionParameters, CallingSessionThreadRunning, Timeout);
    wolthread.detach();

    // build the appropriate WebOS handshake
    if (key == "")
        handshake = narrow(HANDSHAKE_NOTPAIRED);
    else
    {
        handshake = narrow(HANDSHAKE_PAIRED);
        size_t ckf = handshake.find(ck);
        handshake.replace(ckf, ck.length(), key);
    }

    //try waking up the display ten times
    while (time(0) - origtim < (Timeout+1))
    {
        time_t looptim = time(0);
        try
        {
            //BOOST::BEAST
            net::io_context ioc;
            tcp::resolver resolver{ ioc };
            websocket::stream<tcp::socket> ws{ ioc };
            beast::flat_buffer buffer;
            string host = hostip;

             // try communicating with the display
            auto const results = resolver.resolve(host, SERVICE_PORT);
            auto ep = net::connect(ws.next_layer(), results);
            host += ':' + std::to_string(ep.port());

            ws.set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req)
                {
                    req.set(http::field::user_agent,
                        std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-client-LGTVsvc");
                }));
            ws.handshake(host, "/");
            ws.write(net::buffer(std::string(handshake)));
            ws.read(buffer); // read the first response
            string tts = "Response received: ";
            tts += beast::buffers_to_string(buffer.data());
            Log(tts);

            // parse for pairing key if needed
            if (key == "")
            {
                ws.read(buffer); //read the second response which hopefully now contains the session key
                string t = beast::buffers_to_string(buffer.data());
                size_t u = t.find("client-key\":\"");
                if (u != string::npos) // so did we get a session key?
                {
                    size_t w = t.find("\"", u + 14);
 
                    if (w != string::npos)
                    {
                        while (!mMutex.try_lock())
                            Sleep(MUTEX_WAIT);

                        key = t.substr(u + 13, w - u - 13);
                        CallingSessionParameters->SessionKey = key;
                        SetSessionKey(key, CallingSessionParameters->DeviceId);
                        mMutex.unlock();
                    }
                }
            }
            ws.close(websocket::close_code::normal);
            string sd = "DisplayPowerOnThread established contact with ";
            sd += CallingSessionParameters->DeviceId; sd += ". Display is powered ON.";
            Log(sd);
            break;
        }
        catch (std::exception const& e)
        {
            string s = "WARNING! DisplayPowerOnThread: ";
            s += e.what();
            Log(s);
        }
        time_t endtim = time(0);
        time_t execution_time = endtim - looptim;
        if (execution_time < 6)
            Sleep((6- (DWORD)execution_time)*1000);
    }

    while (!mMutex.try_lock())
        Sleep(MUTEX_WAIT);
    *CallingSessionThreadRunning = false;
    mMutex.unlock();
    return;
 
}
//   THREAD: Spawned to broadcast WOL (this is what actually wakes the display up)
void WOLthread (SESSIONPARAMETERS* CallingSessionParameters, bool* CallingSessionThreadRunning, int Timeout)
{
    if (!CallingSessionParameters)
        return;
    if (CallingSessionParameters->MAC.size() < 1)
        return; 
    SOCKET WOLsocket = INVALID_SOCKET;
    try
    {
        //WINSOCKET
        struct sockaddr_in LANDestination {};
        LANDestination.sin_family = AF_INET;
        LANDestination.sin_port = htons(9);
        LANDestination.sin_addr.s_addr = 0xFFFFFFFF;
        vector<string> MACs;
        MACs = CallingSessionParameters->MAC;
        time_t origtim = time(0);

        WOLsocket= socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (WOLsocket == INVALID_SOCKET)
        {
            int dw = WSAGetLastError();
            stringstream ss;
            ss << "WOLthread ERROR! WS socket() error: ";
            ss << dw;
            Log(ss.str());
            return;
        }
        else
        {
            const bool optval = TRUE;
            if (setsockopt(WOLsocket, SOL_SOCKET, SO_BROADCAST, (char*)&optval, sizeof(optval)) == SOCKET_ERROR)
            {
                closesocket(WOLsocket);
                int dw = WSAGetLastError();
                stringstream ss;
                ss << "WOLthread ERROR! WS setsockopt() error: ";
                ss << dw;
                Log(ss.str());
                return;
            }
        }
        for (auto& MAC : MACs)
        {
            string LogMessage = "WOL repeating broadcast started to MAC: ";
            LogMessage += MAC;
            Log(LogMessage);

            //remove ':', ' ' and'-' from MAC
            char CharsToRemove[] = ":- ";
            for (int i = 0; i < strlen(CharsToRemove); ++i)
                MAC.erase(remove(MAC.begin(), MAC.end(), CharsToRemove[i]), MAC.end());
        }

        //send WOL packet every other second until timeout, or until the calling thread has ended
        while (time(0) - origtim < (Timeout + 1))
        {
            time_t looptim = time(0);
            if (WOLsocket != INVALID_SOCKET)
            {
                // build and broadcast magic packet(s) for every MAC
                for (auto& MAC : MACs)
                {
                    //Build a magic packet (6 x 0xFF followed by 16 x MAC) and broadcast it
                    if (MAC.length() == 12)
                    {
                        unsigned char Message[102];
                        unsigned char MACstr[6];

                        for (auto i = 0; i < 6; ++i)
                            Message[i] = 0xFF;

                        for (auto i = 0; i < 6; ++i)
                            MACstr[i] = static_cast<unsigned char>(std::stoul(MAC.substr(i * 2, 2), nullptr, 16));

                        for (auto i = 1; i <= 16; ++i)
                            memcpy(&Message[i * 6], &MACstr, 6 * sizeof(unsigned char));

                        // Send Wake On LAN packet
                        if (sendto(WOLsocket, (char*)&Message, 102, 0, reinterpret_cast<sockaddr*>(&LANDestination), sizeof(LANDestination)) == SOCKET_ERROR)                       
                        {
                            int dw = WSAGetLastError();
                            stringstream ss;
                            ss << "WOLthread WARNING! WS sendto() error: ";
                            ss << dw;
                            Log(ss.str());
                        }
                    }
                    else
                        Log("WOLthread WARNING! Malformed MAC");
                }
            }
            else
                Log("WOLthread WARNING! Illegal socket reference");
           
            time_t endtim = time(0);
            time_t execution_time = endtim - looptim;
            if (execution_time < 2)
                Sleep((2 - (DWORD)execution_time) * 1000);
            
            while (!mMutex.try_lock())
                Sleep(MUTEX_WAIT);
            if (*CallingSessionThreadRunning == false)
            {
                mMutex.unlock();
                break;
            }
            mMutex.unlock();
        }

        if (WOLsocket != INVALID_SOCKET)
        {
            closesocket(WOLsocket);
            WOLsocket = INVALID_SOCKET;
        }
    }
    catch (std::exception const& e)
    {
        string s = "WOLthread ERROR: ";
        s += e.what();
        Log(s);

    }
    Log("WOL repeating broadcast ended");

    if (WOLsocket != INVALID_SOCKET)
        closesocket(WOLsocket);
    return;

}
//   THREAD: Spawned when the device should power OFF.
void DisplayPowerOffThread(SESSIONPARAMETERS* CallingSessionParameters, bool* CallingSessionThreadRunning)
{
    if (!CallingSessionParameters)
        return;
    if (CallingSessionParameters->SessionKey != "") //assume we have paired here. Doe not make sense to try pairing when display shall turn off.
    {
        try
        {
            string host = CallingSessionParameters->IP;
            string key = CallingSessionParameters->SessionKey;
            string poweroffmess = "{ \"id\":\"0\",\"type\" : \"request\",\"uri\" : \"ssap://system/turnOff\"}";
            string handshake = narrow(HANDSHAKE_PAIRED);
            string ck = "CLIENTKEYx0x0x0";
            net::io_context ioc;

            size_t ckf = handshake.find(ck);
            handshake.replace(ckf, ck.length(), key);
            tcp::resolver resolver{ ioc };
            websocket::stream<tcp::socket> ws{ ioc };
            auto const results = resolver.resolve(host, SERVICE_PORT);
            auto ep = net::connect(ws.next_layer(), results);

            host += ':' + std::to_string(ep.port());

            // Set a decorator to change the User-Agent of the handshake
            ws.set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req)
                {
                    req.set(http::field::user_agent,
                        std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-client-LGTVsvc");
                }));
            ws.handshake(host, "/");
 
            beast::flat_buffer buffer;
            ws.write(net::buffer(std::string(handshake)));
            ws.read(buffer); // read the response
            ws.write(net::buffer(std::string(poweroffmess)));
            ws.read(buffer); // read the response
            ws.close(websocket::close_code::normal);
           
            string sd = "DisplayPowerOffThread established contact with ";
            sd += CallingSessionParameters->DeviceId; sd += ". Display is powered OFF.";
            Log(sd);

        }
        catch (std::exception const& e)
        {
            string s = "WARNING: DisplayPowerOffThread failed: ";
            s += e.what();
            Log(s);
        }
    }
    //thread safe section
    while (!mMutex.try_lock())
        Sleep(MUTEX_WAIT);
    *CallingSessionThreadRunning = false;
    mMutex.unlock();
    return ;
}