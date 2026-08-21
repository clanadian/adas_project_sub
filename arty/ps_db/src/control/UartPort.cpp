#include "control/UartPort.hpp"

#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace adas::control {

namespace {

speed_t toSpeed(unsigned baud_rate) {
    switch (baud_rate) {
    case 9600u:   return B9600;
    case 19200u:  return B19200;
    case 38400u:  return B38400;
    case 57600u:  return B57600;
    case 115200u: return B115200;
    case 230400u: return B230400;
    default:      return 0;
    }
}

}  // namespace

PosixUartPort::~PosixUartPort() {
    close();
}

bool PosixUartPort::open(const std::string& path, unsigned baud_rate) {
    if (isOpen() || path.empty()) {
        return false;
    }
    const speed_t speed = toSpeed(baud_rate);
    if (speed == 0) {
        return false;
    }

    /*
     * O_NOCTTY: 이 포트를 프로세스의 제어 터미널로 삼지 않는다. 없으면
     * 시리얼 쪽 신호가 프로세스 그룹에 전달된다.
     */
    const int fd = ::open(path.c_str(), O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    termios options{};
    if (::tcgetattr(fd, &options) != 0) {
        ::close(fd);
        return false;
    }

    ::cfmakeraw(&options);
    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);   /* stop bit 1 */
    options.c_cflag &= ~static_cast<tcflag_t>(PARENB);   /* parity 없음 */
    options.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);  /* flow control 없음 */
    (void)::cfsetispeed(&options, speed);
    (void)::cfsetospeed(&options, speed);

    if (::tcsetattr(fd, TCSANOW, &options) != 0) {
        ::close(fd);
        return false;
    }

    /* 앞선 프로세스가 남긴 바이트가 프레임 앞에 붙지 않게 비운다. */
    (void)::tcflush(fd, TCOFLUSH);

    fd_ = fd;
    return true;
}

void PosixUartPort::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool PosixUartPort::write(const std::uint8_t* data, std::size_t size) {
    if (fd_ < 0 || data == nullptr || size == 0u) {
        return false;
    }

    std::size_t sent = 0u;
    while (sent < size) {
        const ssize_t result = ::write(fd_, data + sent, size - sent);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

}  // namespace adas::control
