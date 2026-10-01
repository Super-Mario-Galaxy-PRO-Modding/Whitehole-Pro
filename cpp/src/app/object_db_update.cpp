#include "whitehole/app/object_db_update.hpp"

#include "whitehole/io/binary_file.hpp"

#include <fstream>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace whitehole::app {
namespace {

#ifdef _WIN32

// Minimal RAII wrapper so every exit path closes its WinHTTP handle.
class InternetHandle {
public:
    InternetHandle() = default;
    ~InternetHandle() { reset(); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }
    void set(HINTERNET handle) noexcept {
        reset();
        handle_ = handle;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_{nullptr};
};

constexpr wchar_t kDatabaseHost[] = L"raw.githubusercontent.com";
constexpr wchar_t kDatabasePath[] = L"/SMGCommunity/galaxydatabase/main/objectdb.json";

bool fetchDatabase(std::vector<char>& payload, std::string& error) {
    InternetHandle session;
    session.set(WinHttpOpen(L"WhiteholePro/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.valid()) {
        error = "could not start an HTTP session (WinHttpOpen " +
                std::to_string(GetLastError()) + ")";
        return false;
    }

    InternetHandle connection;
    connection.set(WinHttpConnect(session.get(), kDatabaseHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.valid()) {
        error = "could not reach the database host (WinHttpConnect " +
                std::to_string(GetLastError()) + ")";
        return false;
    }

    InternetHandle request;
    request.set(WinHttpOpenRequest(connection.get(), L"GET", kDatabasePath, nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE));
    if (!request.valid()) {
        error = "could not build the request (WinHttpOpenRequest " +
                std::to_string(GetLastError()) + ")";
        return false;
    }

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = "could not send the request (WinHttpSendRequest " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "no response from the database host (WinHttpReceiveResponse " +
                std::to_string(GetLastError()) + ")";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = "could not read the HTTP status (WinHttpQueryHeaders " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    if (status != 200) {
        error = "the database host returned HTTP " + std::to_string(status);
        return false;
    }

    payload.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            error = "the download stalled (WinHttpQueryDataAvailable " +
                    std::to_string(GetLastError()) + ")";
            return false;
        }
        if (available == 0) break;

        const std::size_t previous = payload.size();
        payload.resize(previous + static_cast<std::size_t>(available));
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), payload.data() + previous, available, &read)) {
            error = "the download failed (WinHttpReadData " + std::to_string(GetLastError()) + ")";
            return false;
        }
        payload.resize(previous + static_cast<std::size_t>(read));
        if (read == 0) break;
    }
    return !payload.empty();
}

#endif // _WIN32

} // namespace

bool objectDatabaseDownloadAvailable() noexcept {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::string downloadObjectDatabase(const std::filesystem::path& destination) {
#ifndef _WIN32
    (void)destination;
    return std::string("automatic download is only implemented on Windows; fetch ") +
           kObjectDatabaseUrl + " into data/objectdb.json manually";
#else
    std::vector<char> payload;
    std::string error;
    if (!fetchDatabase(payload, error)) {
        return error;
    }
    // Guard against captive portals or error pages being written as a database.
    if (payload.size() < 1024) {
        return "the downloaded database was unexpectedly small";
    }
    if (payload.front() != '{') {
        return "the downloaded data was not the object database";
    }

    std::error_code code;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), code);
    }
    // Commit through writeFile(), which owns the whole safe-save protocol: a
    // per-writer temporary (a FIXED ".download" suffix is a race -- two instances
    // downloading at once opened the same file, and whichever finished first
    // deleted it out from under the other), a .bak of whatever was there, and an
    // in-place copy fallback when the rename cannot happen.
    //
    // It also fixes a real data-loss window this used to have. The old code
    // remove()d the destination and THEN renamed, so a rename that failed -- an
    // antivirus scanner holding the file, a second instance -- left the modder
    // with NO database at all, having destroyed the one they already had.
    // Nothing is staged and removed by hand any more: writeFile does both.
    //
    // Nothing is thrown out of here; this function reports failure as a string,
    // which is what the caller surfaces in a toast.
    //
    // payload is a std::vector<char> (that is what WinHttpReadData fills in) and
    // writeFile takes bytes, so convert once rather than reinterpreting: a
    // reinterpret_cast here would be the one place in this file that could get
    // signedness wrong, and there is no reason to take that risk for a copy.
    std::vector<std::uint8_t> bytes;
    bytes.reserve(payload.size());
    for (const char value : payload) {
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    try {
        io::writeFile(destination, bytes);
    } catch (const std::exception& failure) {
        return std::string("could not write the downloaded database: ") + failure.what();
    }
    return {};
#endif
}

} // namespace whitehole::app