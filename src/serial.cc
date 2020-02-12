/*
 Copyright (C) 2017-2019 Fredrik Öhrström

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

#include"util.h"
#include"serial.h"
#include"shell.h"

#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <memory.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

static int openSerialTTY(const char *tty, int baud_rate);

struct SerialDeviceImp;
struct SerialDeviceTTY;
struct SerialDeviceCommand;
struct SerialDeviceFile;
struct SerialDeviceSimulator;

struct SerialCommunicationManagerImp : public SerialCommunicationManager
{
    SerialCommunicationManagerImp(time_t exit_after_seconds, time_t reopen_after_seconds, bool start_event_loop);
    ~SerialCommunicationManagerImp();

    unique_ptr<SerialDevice> createSerialDeviceTTY(string dev, int baud_rate);
    unique_ptr<SerialDevice> createSerialDeviceCommand(string command, vector<string> args, vector<string> envs,
                                                       function<void()> on_exit);
    unique_ptr<SerialDevice> createSerialDeviceFile(string file);
    unique_ptr<SerialDevice> createSerialDeviceSimulator();

    void listenTo(SerialDevice *sd, function<void()> cb);
    void stop();
    void startEventLoop();
    void waitForStop();
    bool isRunning();
    void setReopenAfter(int seconds);

    void opened(SerialDeviceImp *sd);
    void closed(SerialDeviceImp *sd);
    void closeAll();

    time_t reopenAfter() { return reopen_after_seconds_; }

private:

    void *eventLoop();
    static void *startLoop(void *);

    bool running_ {};
    bool expect_devices_to_work_ {}; // false during detection phase, true when running.
    pthread_t main_thread_ {};
    pthread_t thread_ {};
    int max_fd_ {};
    time_t start_time_ {};
    time_t exit_after_seconds_ {};
    time_t reopen_after_seconds_ {};

    pthread_mutex_t event_loop_lock_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t devices_lock_ = PTHREAD_MUTEX_INITIALIZER;
    vector<SerialDeviceImp*> devices_;
};

SerialCommunicationManagerImp::~SerialCommunicationManagerImp()
{
    // Close all managed devices (not yet closed)
    closeAll();
    // Stop the event loop.
    stop();
    // Grab the event_loop_lock. This can only be done when the eventLoop has stopped running.
    pthread_mutex_lock(&event_loop_lock_);
    // Now we can be sure the eventLoop has stopped and it is safe to
    // free this Manager object.
}

struct SerialDeviceImp : public SerialDevice
{
    int fd() { return fd_; }
    void fill(vector<uchar> &data) {};
    int receive(vector<uchar> *data);
    bool working() { return fd_ != -1; }
    void expectAscii() { expecting_ascii_ = true; }
    void setIsFile() { is_file_ = true; }
    void setIsStdin() { is_stdin_ = true; }

protected:

    pthread_mutex_t read_lock_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t write_lock_ = PTHREAD_MUTEX_INITIALIZER;
    function<void()> on_data_;
    int fd_ = -1;
    bool expecting_ascii_ {}; // If true, print using safeString instead if bin2hex
    bool is_file_ = false;
    bool is_stdin_ = false;

    friend struct SerialCommunicationManagerImp;

    ~SerialDeviceImp() = default;
};

int SerialDeviceImp::receive(vector<uchar> *data)
{
    bool close_me = false;

    pthread_mutex_lock(&read_lock_);
    data->clear();
    int num_read = 0;

    while (true)
    {
        data->resize(num_read+1024);
        int nr = read(fd_, &((*data)[num_read]), 1024);
        if (nr > 0)
        {
            num_read += nr;
        }
        if (nr == 0)
        {
            if (is_stdin_ || is_file_)
            {
                debug("(serial) no more data on fd=%d\n", fd_);
                close_me = true;
            }
            break;
        }
        if (nr < 0)
        {
            if (errno == EINTR && fd_ != -1) continue; // Interrupted try again.
            if (errno == EAGAIN) break;   // No more data available since it would block.
            if (errno == EBADF)
            {
                debug("(serial) got EBADF for fd=%d closing it.\n", fd_);
                close_me = true;
                break;
            }
            break;
        }
    }
    data->resize(num_read);

    if (isDebugEnabled())
    {
        if (expecting_ascii_)
        {
            string msg = safeString(*data);
            debug("(serial) received ascii %s\n", msg.c_str());
        }
        else
        {
            string msg = bin2hex(*data);
            debug("(serial) received binary %s\n", msg.c_str());
        }
    }

    pthread_mutex_unlock(&read_lock_);

    if (close_me) close();

    return num_read;
}

struct SerialDeviceTTY : public SerialDeviceImp
{
    SerialDeviceTTY(string device, int baud_rate, SerialCommunicationManagerImp *manager);
    ~SerialDeviceTTY();

    bool open(bool fail_if_not_ok);
    void close();
    void checkIfShouldReopen();
    bool send(vector<uchar> &data);
    bool working();
    SerialCommunicationManager *manager() { return manager_; }

    private:

    string device_;
    int baud_rate_ {};
    SerialCommunicationManagerImp *manager_;
    time_t start_since_reopen_;
    int reopen_after_ {}; // Reopen the device repeatedly after this number of seconds.
    // Necessary for some less than perfect dongles.
};

SerialDeviceTTY::SerialDeviceTTY(string device, int baud_rate,
                                 SerialCommunicationManagerImp *manager)
{
    device_ = device;
    baud_rate_ = baud_rate;
    manager_ = manager;
    start_since_reopen_ = time(NULL);
    reopen_after_ = manager->reopenAfter();
}

SerialDeviceTTY::~SerialDeviceTTY()
{
    close();
}

bool SerialDeviceTTY::open(bool fail_if_not_ok)
{
    bool ok = checkCharacterDeviceExists(device_.c_str(), fail_if_not_ok);
    if (!ok) {
        debug("(serialtty) character devices does not exist \"%s\"\n", device_.c_str());
        return false;
    }
    fd_ = openSerialTTY(device_.c_str(), baud_rate_);
    if (fd_ == -1) {
        if (fail_if_not_ok) {
            error("Could not open %s with %d baud N81\n", device_.c_str(), baud_rate_);
        } else {
            debug("(serialtty) could not open \"%s\" with %d baud N81\n", device_.c_str(), baud_rate_);
            return false;
        }
    }
    manager_->opened(this);
    verbose("(serialtty) opened %s\n", device_.c_str());
    return true;
}

void SerialDeviceTTY::close()
{
    if (fd_ == -1) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
    manager_->closed(this);
    verbose("(serialtty) closed %s\n", device_.c_str());
}

void SerialDeviceTTY::checkIfShouldReopen()
{
    if (fd_ != -1 && reopen_after_ > 0)
    {
        time_t curr = time(NULL);
        time_t diff = curr-start_since_reopen_;
        int available = 0;

        ioctl(fd_, FIONREAD, &available);
        // Is it time to reopen AND there is no data available for reading?
        if (diff > reopen_after_ && !available)
        {
            start_since_reopen_ = curr;

            debug("(serialtty) reopened after %ld seconds\n", diff);
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = openSerialTTY(device_.c_str(), baud_rate_);
            if (fd_ == -1) {
                error("Could not re-open %s with %d baud N81\n", device_.c_str(), baud_rate_);
            }
        }
    }
}

bool SerialDeviceTTY::send(vector<uchar> &data)
{
    if (data.size() == 0) return true;

    pthread_mutex_lock(&write_lock_);

    bool rc = true;
    int n = data.size();
    int written = 0;
    while (true) {
        int nw = write(fd_, &data[written], n-written);
        if (nw > 0) written += nw;
        if (nw < 0) {
            if (errno==EINTR) continue;
            rc = false;
            goto end;
        }
        if (written == n) break;
    }

    if (isDebugEnabled()) {
        string msg = bin2hex(data);
        debug("(serial %s) sent \"%s\"\n", device_.c_str(), msg.c_str());
    }

    end:
    pthread_mutex_unlock(&write_lock_);
    return rc;
}

bool SerialDeviceTTY::working()
{
    if (fd_ == -1) return false;

    bool working = checkCharacterDeviceExists(device_.c_str(), false);

    if (!working) {
        debug("(serial) device %s is gone\n", device_.c_str());
    }

    return working;
}


struct SerialDeviceCommand : public SerialDeviceImp
{
    SerialDeviceCommand(string command, vector<string> args, vector<string> envs,
                        SerialCommunicationManagerImp *manager,
                        function<void()> on_exit);
    ~SerialDeviceCommand();

    bool open(bool fail_if_not_ok);
    void close();
    void checkIfShouldReopen() {}
    bool send(vector<uchar> &data);
    int available();
    bool working();

    SerialCommunicationManager *manager() { return manager_; }

    private:

    string command_;
    int pid_ {};
    vector<string> args_;
    vector<string> envs_;

    pthread_mutex_t write_lock_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t read_lock_ = PTHREAD_MUTEX_INITIALIZER;
    SerialCommunicationManagerImp *manager_;
    function<void()> on_exit_;
};

SerialDeviceCommand::SerialDeviceCommand(string command,
                                         vector<string> args,
                                         vector<string> envs,
                                         SerialCommunicationManagerImp *manager,
                                         function<void()> on_exit)
{
    command_ = command;
    args_ = args;
    envs_ = envs;
    manager_ = manager;
    on_exit_ = on_exit;
}

SerialDeviceCommand::~SerialDeviceCommand()
{
    close();
}

bool SerialDeviceCommand::open(bool fail_if_not_ok)
{
    expectAscii();
    bool ok = invokeBackgroundShell("/bin/sh", args_, envs_, &fd_, &pid_);
    if (!ok) {
        debug("(serialcmd) could not invoke background shell\n");
        return false;
    }
    manager_->opened(this);
    setIsStdin();
    verbose("(serialcmd) opened %s\n", command_.c_str());
    return true;
}

void SerialDeviceCommand::close()
{
    if (pid_ == 0 && fd_ == -1) return;
    if (pid_ && stillRunning(pid_))
    {
        stopBackgroundShell(pid_);
        pid_ = 0;
    }
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
    manager_->closed(this);
    verbose("(serialcmd) closed %s\n", command_.c_str());
}

bool SerialDeviceCommand::working()
{
    if (fd_ == -1) return false;
    if (!pid_) return false;
    bool r = stillRunning(pid_);
    if (r) return true;
    return false;
}

bool SerialDeviceCommand::send(vector<uchar> &data)
{
    if (data.size() == 0) return true;

    pthread_mutex_lock(&write_lock_);

    bool rc = true;
    int n = data.size();
    int written = 0;
    while (true) {
        int nw = write(fd_, &data[written], n-written);
        if (nw > 0) written += nw;
        if (nw < 0) {
            if (errno==EINTR) continue;
            rc = false;
            goto end;
        }
        if (written == n) break;
    }

    if (isDebugEnabled()) {
        string msg = bin2hex(data);
        debug("(serial %s) sent \"%s\"\n", command_.c_str(), msg.c_str());
    }

    end:
    pthread_mutex_unlock(&write_lock_);
    return rc;
}

struct SerialDeviceFile : public SerialDeviceImp
{
    SerialDeviceFile(string file, SerialCommunicationManagerImp *manager);
    ~SerialDeviceFile();

    bool open(bool fail_if_not_ok);
    void close();
    void checkIfShouldReopen();
    bool send(vector<uchar> &data);
    int available();
    SerialCommunicationManager *manager() { return manager_; }

    private:

    string file_;
    SerialCommunicationManagerImp *manager_;
};

SerialDeviceFile::SerialDeviceFile(string file,
                                   SerialCommunicationManagerImp *manager)
{
    file_ = file;
    manager_ = manager;
}

SerialDeviceFile::~SerialDeviceFile()
{
    close();
}

bool SerialDeviceFile::open(bool fail_if_not_ok)
{
    if (file_ == "stdin")
    {
        fd_ = 0;
        int flags = fcntl(0, F_GETFL);
        flags |= O_NONBLOCK;
        fcntl(0, F_SETFL, flags);
        setIsStdin();
        verbose("(serialfile) reading from stdin\n");
    }
    else
    {
        bool ok = checkFileExists(file_.c_str());
        if (!ok) return false;
        fd_ = ::open(file_.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd_ == -1)
        {
            if (fail_if_not_ok)
            {
                error("Could not open file %s for reading.\n", file_.c_str());
            }
            else
            {
                debug("(serialfile) could not open file %s for reading.\n", file_.c_str());
                return false;
            }
        }
        setIsFile();
        verbose("(serialfile) reading from file %s\n", file_.c_str());
    }
    manager_->opened(this);
    return true;
}

void SerialDeviceFile::close()
{
    if (fd_ == -1) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
    manager_->closed(this);
    verbose("(serialtty) closed %s %d\n", file_.c_str(), fd_);
}

void SerialDeviceFile::checkIfShouldReopen()
{
}

bool SerialDeviceFile::send(vector<uchar> &data)
{
    return true;
}

struct SerialDeviceSimulator : public SerialDeviceImp
{
    SerialDeviceSimulator(SerialCommunicationManagerImp *m) :
        manager_(m) {
        manager_->opened(this);
        verbose("(serialsimulator) opened\n");
    };
    ~SerialDeviceSimulator() { close(); };

    bool open(bool fail_if_not_ok) { return true; };
    void close() { manager_->closed(this); };
    void checkIfShouldReopen() { }
    bool send(vector<uchar> &data) { return true; };
    void fill(vector<uchar> &data) { data_ = data; on_data_(); }; // Fill buffer and trigger callback.

    int receive(vector<uchar> *data)
    {
        *data = data_;
        data_.clear();
        return data->size();
    }
    int available() { return data_.size(); }
    int fd() { return -1; }
    bool working() { return false; } // Only one message that has already been handled! So return false here.

    SerialCommunicationManager *manager() { return manager_; }

    private:

    SerialCommunicationManagerImp *manager_;
    vector<uchar> data_;
};

SerialCommunicationManagerImp::SerialCommunicationManagerImp(time_t exit_after_seconds,
                                                             time_t reopen_after_seconds,
                                                             bool start_event_loop)
{
    running_ = true;
    max_fd_ = 0;
    // Block the event loop until everything is configured.
    if (start_event_loop)
    {
        pthread_mutex_lock(&event_loop_lock_);
        pthread_create(&thread_, NULL, startLoop, this);
    }
    wakeMeUpOnSigChld(thread_);
    start_time_ = time(NULL);
    exit_after_seconds_ = exit_after_seconds;
    reopen_after_seconds_ = reopen_after_seconds;
}

void *SerialCommunicationManagerImp::startLoop(void *a)
{
    auto t = (SerialCommunicationManagerImp*)a;
    return t->eventLoop();
}

unique_ptr<SerialDevice> SerialCommunicationManagerImp::createSerialDeviceTTY(string device,
                                                                              int baud_rate)
{
    return unique_ptr<SerialDevice>(new SerialDeviceTTY(device, baud_rate, this));
}

unique_ptr<SerialDevice> SerialCommunicationManagerImp::createSerialDeviceCommand(string command,
                                                                                  vector<string> args,
                                                                                  vector<string> envs,
                                                                                  function<void()> on_exit)
{
    return unique_ptr<SerialDevice>(new SerialDeviceCommand(command, args, envs, this, on_exit));
}

unique_ptr<SerialDevice> SerialCommunicationManagerImp::createSerialDeviceFile(string file)
{
    return unique_ptr<SerialDevice>(new SerialDeviceFile(file, this));
}

unique_ptr<SerialDevice> SerialCommunicationManagerImp::createSerialDeviceSimulator()
{
    return unique_ptr<SerialDevice>(new SerialDeviceSimulator(this));
}

void SerialCommunicationManagerImp::listenTo(SerialDevice *sd, function<void()> cb)
{
    SerialDeviceImp *si = dynamic_cast<SerialDeviceImp*>(sd);
    if (!si)
    {
        error("Internal error: Invalid serial device passed to listenTo.\n");
    }
    si->on_data_ = cb;
}

void SerialCommunicationManagerImp::stop()
{
    // Notify the main waitForStop thread that we are stopped!
    if (running_ == true)
    {
        debug("(serial) stopping manager\n");
        running_ = false;
        if (main_thread_ != 0)
        {
            if (signalsInstalled())
            {
                if (main_thread_) pthread_kill(main_thread_, SIGUSR2);
                if (thread_) pthread_kill(thread_, SIGUSR1);
            }
        }
    }
}

void SerialCommunicationManagerImp::startEventLoop()
{
    // Release the event loop!
    pthread_mutex_unlock(&event_loop_lock_);
}

void SerialCommunicationManagerImp::waitForStop()
{
    debug("(serial) waiting for stop\n");

    expect_devices_to_work_ = true;
    main_thread_ = pthread_self();
    while (running_)
    {
        pthread_mutex_lock(&devices_lock_);
        size_t s = devices_.size();
        pthread_mutex_unlock(&devices_lock_);

        if (s == 0) {
            break;
        }
        usleep(1000*1000);
    }
    if (signalsInstalled())
    {
        if (thread_) pthread_kill(thread_, SIGUSR1);
    }
    pthread_join(thread_, NULL);

    debug("(serial) closing %d devices\n", devices_.size());
    closeAll();
}

bool SerialCommunicationManagerImp::isRunning()
{
    return running_;
}

void SerialCommunicationManagerImp::setReopenAfter(int seconds)
{
    reopen_after_seconds_ = seconds;
}

void SerialCommunicationManagerImp::opened(SerialDeviceImp *sd)
{
    pthread_mutex_lock(&devices_lock_);
    max_fd_ = max(sd->fd(), max_fd_);
    devices_.push_back(sd);
    if (signalsInstalled())
    {
        if (thread_) pthread_kill(thread_, SIGUSR1);
    }
    pthread_mutex_unlock(&devices_lock_);
}

void SerialCommunicationManagerImp::closed(SerialDeviceImp *sd)
{
    pthread_mutex_lock(&devices_lock_);
    auto p = find(devices_.begin(), devices_.end(), sd);
    if (p != devices_.end())
    {
        devices_.erase(p);
    }
    max_fd_ = 0;
    for (SerialDevice *d : devices_)
    {
        if (d->fd() > max_fd_)
        {
            max_fd_ = d->fd();
        }
    }
    if (devices_.size() == 0 && expect_devices_to_work_)
    {
        debug("(serial) no devices working\n");
        stop();
    }
    pthread_mutex_unlock(&devices_lock_);
}

void SerialCommunicationManagerImp::closeAll()
{
    pthread_mutex_lock(&devices_lock_);
    vector<SerialDeviceImp*> copy = devices_;
    pthread_mutex_unlock(&devices_lock_);

    for (SerialDeviceImp *d : copy)
    {
        closed(d);
    }
}

void *SerialCommunicationManagerImp::eventLoop()
{
    fd_set readfds;

    pthread_mutex_lock(&event_loop_lock_);

    while (running_)
    {
        FD_ZERO(&readfds);

        bool all_working = true;
        pthread_mutex_lock(&devices_lock_);
        for (SerialDevice *d : devices_)
        {
            FD_SET(d->fd(), &readfds);
            if (!d->working()) all_working = false;
        }
        pthread_mutex_unlock(&devices_lock_);

        if (!all_working)
        {
            stop();
            break;
        }

        struct timeval timeout { 10, 0 };

        if (exit_after_seconds_ > 0)
        {
            time_t curr = time(NULL);
            time_t diff = curr-start_time_;
            if (diff > exit_after_seconds_) {
                verbose("(serial) exit after %ld seconds\n", diff);
                stop();
                break;
            }
            timeout.tv_sec = exit_after_seconds_ - diff;
        }

        bool num_devices = 0;
        pthread_mutex_lock(&devices_lock_);
        for (SerialDevice *d : devices_)
        {
            d->checkIfShouldReopen();
        }
        num_devices = devices_.size();
        pthread_mutex_unlock(&devices_lock_);

        if (num_devices == 0 && expect_devices_to_work_)
        {
            debug("(serial) no working devices, stopping before entering select.\n");
            stop();
            break;
        }

        int activity = select(max_fd_+1 , &readfds, NULL , NULL, &timeout);
        if (!running_) break;
        if (activity < 0 && errno!=EINTR)
        {
            warning("(serial) internal error after select! errno=%s\n", strerror(errno));
        }
        if (activity > 0)
        {
            // Something has happened that caused the sleeping select to wake up.
            vector<SerialDeviceImp*> to_be_notified;
            pthread_mutex_lock(&devices_lock_);
            for (SerialDevice *d : devices_)
            {
                if (FD_ISSET(d->fd(), &readfds))
                {
                    SerialDeviceImp *si = dynamic_cast<SerialDeviceImp*>(d);
                    to_be_notified.push_back(si);
                }
            }
            pthread_mutex_unlock(&devices_lock_);

            for (SerialDeviceImp *si : to_be_notified)
            {
                if (si->on_data_)
                {
                    si->on_data_();
                }
            }
        }
        vector<SerialDeviceImp*> non_working;

        pthread_mutex_lock(&devices_lock_);
        for (SerialDeviceImp *d : devices_)
        {
            if (!d->working()) non_working.push_back(d);
        }
        pthread_mutex_unlock(&devices_lock_);

        for (SerialDeviceImp *d : non_working)
        {
            debug("(serial) closing non working fd=%d\n", d->fd());
            d->close();
        }
        if (non_working.size() > 0)
        {
            stop();
            break;
        }
    }
    verbose("(serial) event loop stopped!\n");
    pthread_mutex_unlock(&event_loop_lock_);
    return NULL;
}

unique_ptr<SerialCommunicationManager> createSerialCommunicationManager(time_t exit_after_seconds,
                                                                        time_t reopen_after_seconds,
                                                                        bool start_event_loop)
{
    return unique_ptr<SerialCommunicationManager>(new SerialCommunicationManagerImp(exit_after_seconds,
                                                                                    reopen_after_seconds,
                                                                                    start_event_loop));
}

static int openSerialTTY(const char *tty, int baud_rate)
{
    int rc = 0;
    speed_t speed = 0;
    struct termios tios;

    int fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd == -1) {
        usleep(1000*1000);
        fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd == -1) goto err;
    }
    rc = flock(fd, LOCK_EX | LOCK_NB);
    if (rc == -1)
    {
        // It is already locked by another wmbusmeter process.
        warning("Device %s is already in use and locked.\n", tty);
        goto err;
    }

    switch (baud_rate)
    {
    case 9600:   speed = B9600;  break;
    case 19200:  speed = B19200; break;
    case 38400:  speed = B38400; break;
    case 57600:  speed = B57600; break;
    case 115200: speed = B115200;break;
    default:
        goto err;
    }

    memset(&tios, 0, sizeof(tios));

    rc = cfsetispeed(&tios, speed);
    if (rc < 0) goto err;
    rc = cfsetospeed(&tios, speed);
    if (rc < 0) goto err;


    tios.c_cflag |= (CREAD | CLOCAL);
    tios.c_cflag &= ~CSIZE;
    tios.c_cflag |= CS8;
    tios.c_cflag &=~ CSTOPB;
    tios.c_cflag &=~ PARENB;

    tios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tios.c_iflag &= ~INPCK;

    tios.c_iflag &= ~(IXON | IXOFF | IXANY);

    tios.c_oflag &=~ OPOST;
    tios.c_cc[VMIN] = 0;
    tios.c_cc[VTIME] = 0;

    rc = tcsetattr(fd, TCSANOW, &tios);
    if (rc < 0) goto err;

    return fd;

err:
    if (fd != -1) close(fd);
    return -1;
}

SerialCommunicationManager::~SerialCommunicationManager()
{
}
