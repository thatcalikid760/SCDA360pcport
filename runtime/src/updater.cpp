#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <runtime.h>
#include <settings.h>
#include "updater.h"

#ifndef SCDA_VERSION
#define SCDA_VERSION "0.0.0"
#endif
#ifndef SCDA_UPDATE_REPO
#define SCDA_UPDATE_REPO "thatcalikid760/SCDA360pcport"
#endif

namespace updater {

namespace {

std::mutex g_mutex;
Status g_status;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// GET https://api.github.com<path>; false with `error` set when no HTTP answer came back.
bool HttpGet(const std::string& path, DWORD& statusCode, std::string& body, std::string& error)
{
    const std::wstring agent = Widen(std::string("SCDARecomp/") + SCDA_VERSION); // GitHub refuses requests without one
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", Widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = false;
    if (request)
    {
        WinHttpSetTimeouts(request, 5000, 5000, 10000, 10000);
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        DWORD size = sizeof(statusCode);
        if (WinHttpSendRequest(request, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
                                WINHTTP_NO_HEADER_INDEX))
        {
            ok = true;
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail) || !avail || body.size() > (1u << 20))
                    break;
                std::vector<char> chunk(avail);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read) || !read)
                    break;
                body.append(chunk.data(), read);
            }
        }
    }
    if (!ok)
        error = "no connection to GitHub (error " + std::to_string(GetLastError()) + ")";
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    if (session)
        WinHttpCloseHandle(session);
    return ok;
}

void Set(const Status& s)
{
    std::lock_guard lock(g_mutex);
    g_status = s;
}

void RunCheck()
{
    Status s;
    DWORD code = 0;
    std::string body, error;
    if (!HttpGet("/repos/" + Repo() + "/releases/latest", code, body, error))
    {
        s.state = State::Failed;
        s.error = error;
    }
    else if (code == 404)
        s.state = State::NoReleases; // releases/latest ignores drafts and prereleases
    else if (code != 200)
    {
        s.state = State::Failed;
        s.error = "GitHub answered " + std::to_string(code) + (code == 403 || code == 429 ? " (rate limit, try later)" : "");
    }
    else
    {
        s.latest = JsonString(body, "tag_name");
        if (!s.latest.empty() && (s.latest[0] == 'v' || s.latest[0] == 'V'))
            s.latest.erase(0, 1);
        s.title = JsonString(body, "name");
        if (s.latest.empty())
        {
            s.state = State::Failed;
            s.error = "the release has no tag";
        }
        else
            s.state = CompareVersions(s.latest, CurrentVersion()) > 0 ? State::Available : State::UpToDate;
    }
    RuntimeLog("[update] %s: state %d latest '%s'%s%s\n", Repo().c_str(), int(s.state), s.latest.c_str(), s.error.empty() ? "" : " ", s.error.c_str());
    Set(s);
}

} // namespace

const char* CurrentVersion() { return SCDA_VERSION; }

const std::string& Repo()
{
    static const std::string repo = []
    {
        std::string r = LoadUpdateSettings(SettingsPath()).repo;
        return r.empty() ? std::string(SCDA_UPDATE_REPO) : r;
    }();
    return repo;
}

std::string ReleasePageUrl() { return "https://github.com/" + Repo() + "/releases/latest"; }

Status Get()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}

void Check()
{
    {
        std::lock_guard lock(g_mutex);
        if (g_status.state == State::Checking)
            return;
        g_status = Status{ State::Checking };
    }
    std::thread(RunCheck).detach();
}

void OpenReleasePage()
{
    const std::wstring url = Widen(ReleasePageUrl());
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int CompareVersions(const std::string& a, const std::string& b)
{
    size_t i = !a.empty() && (a[0] == 'v' || a[0] == 'V'), j = !b.empty() && (b[0] == 'v' || b[0] == 'V');
    while (i < a.size() || j < b.size())
    {
        unsigned long x = 0, y = 0;
        while (i < a.size() && isdigit((unsigned char)a[i]))
            x = x * 10 + unsigned(a[i++] - '0');
        while (j < b.size() && isdigit((unsigned char)b[j]))
            y = y * 10 + unsigned(b[j++] - '0');
        if (x != y)
            return x < y ? -1 : 1;
        // Next field: past the '.', stop at anything else (a "-rc1" suffix is ignored).
        i = i < a.size() && a[i] == '.' ? i + 1 : a.size();
        j = j < b.size() && b[j] == '.' ? j + 1 : b.size();
    }
    return 0;
}

std::string JsonString(const std::string& json, const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    size_t p = json.find(quoted);
    if (p == std::string::npos)
        return {};
    p = json.find_first_not_of(" \t\r\n", p + quoted.size());
    if (p == std::string::npos || json[p] != ':')
        return {};
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '"')
        return {};
    std::string out;
    for (p++; p < json.size() && json[p] != '"'; p++)
    {
        if (json[p] == '\\' && p + 1 < json.size())
        {
            const char e = json[++p];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'u' ? '?' : e;
            if (e == 'u')
                p += 4; // non-ASCII in a release name: shown as '?'
        }
        else
            out += json[p];
    }
    return out;
}

} // namespace updater
