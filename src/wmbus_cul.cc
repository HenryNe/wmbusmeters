/*
 Copyright (C) 2019-2020 Fredrik Öhrström

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include"wmbus.h"
#include"wmbus_utils.h"
#include"wmbus_cul.h"
#include"serial.h"

#include<assert.h>
#include<fcntl.h>
#include<grp.h>
#include<pthread.h>
#include<semaphore.h>
#include<string.h>
#include<sys/errno.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<unistd.h>

using namespace std;

enum FrameStatus { PartialFrame, FullFrame, ErrorInFrame, TextAndNotFrame };

#define SET_LINK_MODE 1
#define SET_X01_MODE 2

struct WMBusCUL : public virtual WMBusCommonImplementation
{
    bool ping();
    uint32_t getDeviceId();
    LinkModeSet getLinkModes();
    void setLinkModes(LinkModeSet lms);
    LinkModeSet supportedLinkModes() {
        return
            C1_bit |
            S1_bit |
            T1_bit;
    }
    int numConcurrentLinkModes() { return 1; }
    bool canSetLinkModes(LinkModeSet lms)
    {
        if (0 == countSetBits(lms.bits())) return false;
        if (!supportedLinkModes().supports(lms)) return false;
        // Ok, the supplied link modes are compatible,
        // but im871a can only listen to one at a time.
        return 1 == countSetBits(lms.bits());
    }
    void processSerialData();
    SerialDevice *serial() { return serial_.get(); }
    void simulate();

    WMBusCUL(unique_ptr<SerialDevice> serial, SerialCommunicationManager *manager);
    ~WMBusCUL() { }

private:
    unique_ptr<SerialDevice> serial_;
    SerialCommunicationManager *manager_ {};
    LinkModeSet link_modes_ {};
    vector<uchar> read_buffer_;
    vector<uchar> received_payload_;
    sem_t command_wait_;
    string sent_command_;
    string received_response_;

    void waitForResponse();

    FrameStatus checkCULFrame(vector<uchar> &data,
                                   size_t *hex_frame_length,
                                   int *hex_payload_len_out,
                                   int *hex_payload_offset);

    string setup_;
};

unique_ptr<WMBus> openCUL(string device, SerialCommunicationManager *manager, unique_ptr<SerialDevice> serial_override)
{
    if (serial_override)
    {
        WMBusCUL *imp = new WMBusCUL(std::move(serial_override), manager);
        return unique_ptr<WMBus>(imp);
    }

    auto serial = manager->createSerialDeviceTTY(device.c_str(), 38400);
    WMBusCUL *imp = new WMBusCUL(std::move(serial), manager);
    return unique_ptr<WMBus>(imp);
}

WMBusCUL::WMBusCUL(unique_ptr<SerialDevice> serial, SerialCommunicationManager *manager) :
    WMBusCommonImplementation(DEVICE_CUL), serial_(std::move(serial)), manager_(manager)
{
    sem_init(&command_wait_, 0, 0);
    manager_->listenTo(serial_.get(),call(this,processSerialData));
    serial_->open(true);
}

bool WMBusCUL::ping()
{
    verbose("(cul) ping\n");
    return true;
}

uint32_t WMBusCUL::getDeviceId()
{
    verbose("(cul) getDeviceId\n");
    return 0x11111111;
}

LinkModeSet WMBusCUL::getLinkModes()
{
    return link_modes_;
}

void WMBusCUL::setLinkModes(LinkModeSet lms)
{
    if (!canSetLinkModes(lms))
    {
        string modes = lms.hr();
        error("(cul) setting link mode(s) %s is not supported\n", modes.c_str());
    }
    // 'brc' command: b - wmbus, r - receive, c - c mode (with t)
    vector<uchar> msg(5);
    msg[0] = 'b';
    msg[1] = 'r';
    if (lms.has(LinkMode::C1)) {
        msg[2] = 'c';
    } else if (lms.has(LinkMode::S1)) {
        msg[2] = 's';
    } else if (lms.has(LinkMode::T1)) {
        msg[2] = 't';
    }
    msg[3] = 0xa;
    msg[4] = 0xd;

    verbose("(cul) set link mode %c\n", msg[2]);
    sent_command_ = string(&msg[0], &msg[3]);
    received_response_ = "";
    bool sent = serial()->send(msg);

    if (sent) waitForResponse();

    sent_command_ = "";
    debug("(cul) received \"%s\"", received_response_.c_str());

    bool ok = true;
    if (lms.has(LinkMode::C1)) {
        if (received_response_ != "CMODE") ok = false;
    } else if (lms.has(LinkMode::S1)) {
        if (received_response_ != "SMODE") ok = false;
    } else if (lms.has(LinkMode::T1)) {
        if (received_response_ != "TMODE") ok = false;
    }

    if (!ok)
    {
        string modes = lms.hr();
        error("(cul) setting link mode(s) %s is not supported for this cul device!\n", modes.c_str());
    }

    // Remember the link modes, necessary when using stdin or file.
    link_modes_ = lms;

    // X01 - start the receiver
    msg[0] = 'X';
    msg[1] = '0';
    msg[2] = '1';
    msg[3] = 0xa;
    msg[4] = 0xd;

    sent = serial()->send(msg);

    // Any response here, or does it silently move into listening mode?
}

void WMBusCUL::waitForResponse()
{
    while (manager_->isRunning())
    {
        int rc = sem_wait(&command_wait_);
        if (rc==0) break;
        if (rc==-1) {
            if (errno==EINTR) continue;
            break;
        }
    }
}

void WMBusCUL::simulate()
{
}

string expectedResponses(vector<uchar> &data)
{
    string safe = safeString(data);
    if (safe.find("CMODE") != string::npos) return "CMODE";
    if (safe.find("TMODE") != string::npos) return "TMODE";
    if (safe.find("SMODE") != string::npos) return "SMODE";
    return "";
}

void WMBusCUL::processSerialData()
{
    vector<uchar> data;

    // Receive and accumulated serial data until a full frame has been received.
    serial_->receive(&data);
    read_buffer_.insert(read_buffer_.end(), data.begin(), data.end());

    size_t frame_length;
    int hex_payload_len, hex_payload_offset;

    for (;;)
    {
        FrameStatus status = checkCULFrame(read_buffer_, &frame_length, &hex_payload_len, &hex_payload_offset);

        if (status == PartialFrame)
        {
            break;
        }
        if (status == TextAndNotFrame)
        {
            // The buffer has already been printed by serial cmd.
            if (sent_command_ != "")
            {
                string r = expectedResponses(read_buffer_);
                if (r != "")
                {
                    received_response_ = r;
                    sem_post(&command_wait_);
                }
            }
            read_buffer_.clear();
            break;
        }
        if (status == ErrorInFrame)
        {
            debug("(cul) error in received message.\n");
            string msg = bin2hex(read_buffer_);
            read_buffer_.clear();
            break;
        }
        if (status == FullFrame)
        {
            vector<uchar> payload;
            if (hex_payload_len > 0)
            {
                vector<uchar> hex;
                hex.insert(hex.end(), read_buffer_.begin()+hex_payload_offset, read_buffer_.begin()+hex_payload_offset+hex_payload_len);
                bool ok = hex2bin(hex, &payload);
                if (!ok)
                {
                    if (hex.size() % 2 == 1)
                    {
                        payload.clear();
                        warning("(cul) warning: the hex string is not an even multiple of two! Dropping last char.\n");
                        hex.pop_back();
                        ok = hex2bin(hex, &payload);
                    }
                    if (!ok)
                    {
                        warning("(cul) warning: the hex string contains bad characters! Decode stopped partway.\n");
                    }
                }
            }

            read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin()+frame_length);

            handleTelegram(payload);
        }
    }
}

FrameStatus WMBusCUL::checkCULFrame(vector<uchar> &data,
                                    size_t *hex_frame_length,
                                    int *hex_payload_len_out,
                                    int *hex_payload_offset)
{
    if (data.size() == 0) return PartialFrame;

    debugPayload("(cul) checkCULFrame", data);

    size_t eolp = 0;
    // Look for end of line
    for (; eolp < data.size(); ++eolp) {
        if (data[eolp] == '\n') break;
    }
    if (eolp >= data.size())
    {
        debug("(cul) no eol found yet, partial frame\n");
        return PartialFrame;
    }

    if (data[0] != 'b')
    {
        // C1 and T1 telegrams should start with a 'b'
        debug("(cul) text and no frame\n");
        return TextAndNotFrame;
    }

    int fix = 0;
    if (data[1] != 'Y')
    {
        // No Y means this is a T1 telegram.
        fix = -1;
    }

    // we received a full C1 frame, TODO check len

    // skip the crc bytes adjusting the length byte by 2
    data[3] -= 2;

    // remove 8: 2 ('bY') + 4 (CRC) + 2 (CRLF) and start at 2
    // remove 7: 1 ('b') + 4 (CRC) + 2 (CRLF) and start at 1
    *hex_frame_length = data.size();
    *hex_payload_len_out = data.size()-8+fix;
    *hex_payload_offset = 2+fix;

    debug("(cul) received full frame\n");
    return FullFrame;
}

bool detectCUL(string device, SerialCommunicationManager *manager)
{
    // Talk to the device and expect a very specific answer.
    auto serial = manager->createSerialDeviceTTY(device.c_str(), 38400);
    bool ok = serial->open(false);
    if (!ok) {
        debug("(cul) could not open tty %s for reading.\n", device.c_str());
        return false;
    }

    vector<uchar> data;
    // send '-'+CRLF -> should be an unsupported command for CUL
    // it should respond with "? (- is unknown) Use one of ..."
    vector<uchar> crlf(3);
    crlf[0] = '-';
    crlf[1] = 0x0d;
    crlf[2] = 0x0a;

    serial->send(crlf);
    usleep(1000*100);
    serial->receive(&data);

    if (data[0] != '?') {
       // no CUL device detected
       serial->close();
       return false;
    }

    data.clear();

    // get the version string: "V 1.67 nanoCUL868" or similar
    vector<uchar> msg(3);
    msg[0] = CMD_GET_VERSION;
    msg[1] = 0x0a;
    msg[2] = 0x0d;

    verbose("(cul) are you there?\n");
    serial->send(msg);
    // Wait for 200ms so that the USB stick have time to prepare a response.
    usleep(1000*200);
    serial->receive(&data);
    string strC(data.begin(), data.end());
    verbose("CUL answered: %s", strC.c_str());

    // TODO: check version string somehow

    serial->close();
    return true;
}
