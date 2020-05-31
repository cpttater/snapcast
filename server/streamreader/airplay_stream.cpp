/***
    This file is part of snapcast
    Copyright (C) 2014-2020  Johannes Pohl

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
***/

#include "airplay_stream.hpp"
#include "base64.h"
#include "common/aixlog.hpp"
#include "common/snap_exception.hpp"
#include "common/utils.hpp"
#include "common/utils/string_utils.hpp"

using namespace std;

namespace streamreader
{

static constexpr auto LOG_TAG = "AirplayStream";

namespace
{
string hex2str(string input)
{
    typedef unsigned char byte;
    unsigned long x = strtoul(input.c_str(), nullptr, 16);
    byte a[] = {byte(x >> 24), byte(x >> 16), byte(x >> 8), byte(x), 0};
    return string((char*)a);
}
} // namespace

/*
 * Expat is used in metadata parsing from Shairport-sync.
 * Without HAS_EXPAT defined no parsing will occur.
 */

AirplayStream::AirplayStream(PcmListener* pcmListener, boost::asio::io_context& ioc, const StreamUri& uri)
    : ProcessStream(pcmListener, ioc, uri), port_(5000), pipe_open_timer_(ioc)
{
    logStderr_ = true;

    string devicename = uri_.getQuery("devicename", "Snapcast");
    params_wo_port_ = "\"--name=" + devicename + "\" --output=stdout --use-stderr --get-coverart";

    port_ = cpt::stoul(uri_.getQuery("port", "5000"));
    setParamsAndPipePathFromPort();

#ifdef HAS_EXPAT
    createParser();
    metadata_dirty_ = false;
    metadata_ = json();
    metadata_["ALBUM"] = "";
    metadata_["ARTIST"] = "";
    metadata_["TITLE"] = "";
    metadata_["COVER"] = "";
#else
    LOG(INFO, LOG_TAG) << "Metadata support not enabled (HAS_EXPAT not defined)"
                       << "\n";
#endif
}


AirplayStream::~AirplayStream()
{
#ifdef HAS_EXPAT
    parse(string("</metatags>"));
    XML_ParserFree(parser_);
#endif
}

#ifdef HAS_EXPAT
int AirplayStream::parse(string line)
{
    enum XML_Status result;

    if ((result = XML_Parse(parser_, line.c_str(), line.length(), false)) == XML_STATUS_ERROR)
    {
        XML_ParserFree(parser_);
        createParser();
    }
    return result;
}

void AirplayStream::createParser()
{
    parser_ = XML_ParserCreate("UTF-8");
    XML_SetElementHandler(parser_, element_start, element_end);
    XML_SetCharacterDataHandler(parser_, data);
    XML_SetUserData(parser_, this);

    // Make an outer element to keep parsing going
    parse(string("<metatags>"));
}

void AirplayStream::push()
{
    // The metadata we collect consists of two parts:
    // (1) ALBUM, ARTIST, TITLE
    // (2) COVER
    //
    // This stems from the Airplay protocol, which treats cover art differently from the rest of the metadata.
    //
    // The process for (1) is as follows:
    // - The ssnc->mdst message is sent ("metadata start")
    // - core->asal|asar|minm messages are sent
    // - The ssnc->mden message is sent ("metadata end")
    // This process can repeat multiple times *for the same song*, with *the same metadata*.
    //
    // The process for (2) is as follows:
    // - The ssnc->pcst message is sent ("picture start")
    // - The ssnc->PICT message is sent (picture contents)
    // - The ssnc->pcen message is sent ("picture end")
    // If no cover art is available, the PICT message's data has a length of 0 *or* none of the messages are sent.
    //
    // Here is an example from an older iPad:
    //
    // User plays song without cover art
    // - empty cover art message (2)
    // - empty cover art message (2)
    // - metadata message (1)
    // - metadata message (1)
    // - metadata message (1)
    // User selects next song without cover art
    // - metadata message (1)
    // - metadata message (1)
    // User selects next song with cover art
    // - metadata message (1)
    // - metadata message (1)
    // - cover art message (2)
    // - metadata message (1)
    // User selects next song with cover art
    // - metadata message (1)
    // - metadata message (1)
    // - empty cover art message (2) (!)
    // - metadata message (1)
    // - cover art message (2)
    //
    // As can be seen, the order of metadata (1) and cover (2) messages is non-deterministic.
    // That is why we call setMeta() on both end of message (1) and (2).
    string data = entry_->data;

    // Do not base64 decode cover art
    const bool is_cover = entry_->type == "ssnc" && entry_->code == "PICT";
    if (!is_cover && entry_->isBase64 && entry_->length > 0)
        data = base64_decode(data);

    if (is_cover)
    {
        setMetaData("COVER", data);
        // LOG(INFO, LOG_TAG) << "Metadata type: " << entry_->type << " code: " << entry_->code << " data length: " << data.length() << "\n";
    }
    else
    {
        // LOG(INFO, LOG_TAG) << "Metadata type: " << entry_->type << " code: " << entry_->code << " data: " << data << "\n";
    }

    if (entry_->type == "core" && entry_->code == "asal")
        setMetaData("ALBUM", data);
    if (entry_->type == "core" && entry_->code == "asar")
        setMetaData("ARTIST", data);
    if (entry_->type == "core" && entry_->code == "minm")
        setMetaData("TITLE", data);

    // mden = metadata end, pcen == picture end
    if (metadata_dirty_ && entry_->type == "ssnc" && (entry_->code == "mden" || entry_->code == "pcen"))
    {
        setMeta(metadata_);
        metadata_dirty_ = false;
    }
}

void AirplayStream::setMetaData(const string& key, const string& newValue)
{
    // Only overwrite metadata and set metadata_dirty_ if the metadata has changed.
    // This avoids multiple unnecessary transmissions of the same metadata.
    const auto& oldValue = metadata_[key];
    if (oldValue != newValue)
    {
        metadata_[key] = newValue;
        metadata_dirty_ = true;
    }
}
#endif


void AirplayStream::setParamsAndPipePathFromPort()
{
    pipePath_ = "/tmp/shairmeta." + cpt::to_string(getpid()) + "." + cpt::to_string(port_);
    params_ = params_wo_port_ + " \"--metadata-pipename=" + pipePath_ + "\" --port=" + cpt::to_string(port_);
}


void AirplayStream::do_connect()
{
    ProcessStream::do_connect();
    pipeReadLine();
}


void AirplayStream::pipeReadLine()
{
    if (!pipe_fd_ || !pipe_fd_->is_open())
    {
        try
        {
            int fd = open(pipePath_.c_str(), O_RDONLY | O_NONBLOCK);
            pipe_fd_ = std::make_unique<boost::asio::posix::stream_descriptor>(ioc_, fd);
            LOG(INFO, LOG_TAG) << "Metadata pipe opened: " << pipePath_ << "\n";
        }
        catch (const std::exception& e)
        {
            LOG(ERROR, LOG_TAG) << "Error opening metadata pipe, retrying in 500ms. Error: " << e.what() << "\n";
            pipe_fd_ = nullptr;
            wait(pipe_open_timer_, 500ms, [this] { pipeReadLine(); });
            return;
        }
    }

    const std::string delimiter = "\n";
    boost::asio::async_read_until(*pipe_fd_, streambuf_pipe_, delimiter, [this, delimiter](const std::error_code& ec, std::size_t bytes_transferred) {
        if (ec)
        {
            if (ec.value() == boost::asio::error::eof)
            {
                // For some reason, EOF is returned until the first metadata is written to the pipe.
                // Is this a boost bug?
                LOG(INFO, LOG_TAG) << "Waiting for metadata, retrying in 2500ms"
                                   << "\n";
                wait(pipe_open_timer_, 2500ms, [this] { pipeReadLine(); });
            }
            if (ec.value() == boost::asio::error::bad_descriptor)
            {
                // If shairport-sync has not finished setting up the pipe, bad file descriptor is returned. 
                // retry in a few seconds
		LOG(INFO, LOG_TAG) << "Waiting for metadata, retrying in 2500ms"
                                   << "\n";
                wait(pipe_open_timer_, 2500ms, [this] { pipeReadLine(); });
            }
            else
            {
		LOG(ERROR, LOG_TAG) << "Error while reading from metadata pipe: " << ec.message() << "\n";
            }
            return;
        }

        // Extract up to the first delimiter.
        std::string line{buffers_begin(streambuf_pipe_.data()), buffers_begin(streambuf_pipe_.data()) + bytes_transferred - delimiter.length()};
        if (!line.empty())
        {
            if (line.back() == '\r')
                line.resize(line.size() - 1);
#ifdef HAS_EXPAT
            parse(line);
#endif
        }
        streambuf_pipe_.consume(bytes_transferred);
        pipeReadLine();
    });
}

void AirplayStream::initExeAndPath(const string& filename)
{
    path_ = "";
    exe_ = findExe(filename);
    if (!fileExists(exe_) || (exe_ == "/"))
    {
        exe_ = findExe("shairport-sync");
        if (!fileExists(exe_))
            throw SnapException("shairport-sync not found");
    }

    if (exe_.find("/") != string::npos)
    {
        path_ = exe_.substr(0, exe_.find_last_of("/") + 1);
        exe_ = exe_.substr(exe_.find_last_of("/") + 1);
    }
}


void AirplayStream::onStderrMsg(const std::string& line)
{
    if (line.empty())
        return;
    LOG(INFO, LOG_TAG) << "(" << getName() << ") " << line << "\n";
    if (line.find("Is another instance of Shairport Sync running on this device") != string::npos)
    {
        LOG(ERROR, LOG_TAG) << "It seems there is another Shairport Sync runnig on port " << port_ << ", switching to port " << port_ + 1 << "\n";
        ++port_;
        setParamsAndPipePathFromPort();
    }
    else if (line.find("Invalid audio output specified") != string::npos)
    {
        LOG(ERROR, LOG_TAG) << "shairport sync compiled without stdout audio backend\n";
        LOG(ERROR, LOG_TAG) << "build with: \"./configure --with-stdout --with-avahi --with-ssl=openssl --with-metadata\"\n";
    }
}

#ifdef HAS_EXPAT
void XMLCALL AirplayStream::element_start(void* userdata, const char* element_name, const char** attr)
{
    AirplayStream* self = (AirplayStream*)userdata;
    string name(element_name);

    self->buf_.assign("");
    if (name == "item")
        self->entry_.reset(new TageEntry);

    for (int i = 0; attr[i]; i += 2)
    {
        string name(attr[i]);
        string value(attr[i + 1]);
        if (name == "encoding")
            self->entry_->isBase64 = (value == "base64"); // Quick & dirty..
    }
}

void XMLCALL AirplayStream::element_end(void* userdata, const char* element_name)
{
    AirplayStream* self = (AirplayStream*)userdata;
    string name(element_name);

    if (name == "code")
        self->entry_->code.assign(hex2str(self->buf_));

    else if (name == "type")
        self->entry_->type.assign(hex2str(self->buf_));

    else if (name == "length")
        self->entry_->length = strtoul(self->buf_.c_str(), 0, 10);

    else if (name == "data")
        self->entry_->data = self->buf_;

    else if (name == "item")
        self->push();

    else if (name == "metatags")
        ;
    else
        cout << "Unknown tag <" << name << ">\n";
}

void XMLCALL AirplayStream::data(void* userdata, const char* content, int length)
{
    AirplayStream* self = (AirplayStream*)userdata;
    string value(content, (size_t)length);
    self->buf_.append(value);
}

#endif

} // namespace streamreader
