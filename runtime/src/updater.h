// Update check (F1 menu, Update tab, and a notification at startup): the latest release of the
// port's GitHub repository (SCDA_UPDATE_REPO, owner/name; [Update] Repo= in settings.ini overrides
// it), compared with this build's version (SCDA_VERSION). Only a strictly higher version counts as
// an update. Nothing is downloaded: the menu opens the release page in the browser.
#pragma once
#include <string>

namespace updater {

enum class State { Idle, Checking, UpToDate, Available, NoReleases, Failed };

struct Status
{
    State state = State::Idle;
    std::string latest; // the release's version ("0.2.0", tag without a leading v), once checked
    std::string title;  // the release's name
    std::string error;  // Failed: why
};

const char* CurrentVersion();
const std::string& Repo();
// https://github.com/<repo>/releases/latest (GitHub sends it to the release list while there is none).
std::string ReleasePageUrl();
Status Get();
void Check(); // in the background; ignored while a check runs
void OpenReleasePage();

// Pure helpers (tests).
// <0, 0, >0 like strcmp; numeric fields ("0.10.0" > "0.9.1"), a leading v ignored, missing fields 0.
int CompareVersions(const std::string& a, const std::string& b);
// The string value of "key" in a GitHub API JSON object (no nesting needed), unescaped; empty if absent.
std::string JsonString(const std::string& json, const std::string& key);

} // namespace updater
