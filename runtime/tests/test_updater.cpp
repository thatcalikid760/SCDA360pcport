#include "check.h"
#include <updater.h>

TEST(updater_compare_versions)
{
    CHECK(updater::CompareVersions("0.2.0", "0.1.9") > 0);
    CHECK(updater::CompareVersions("v0.10.0", "0.9.1") > 0);
    CHECK(updater::CompareVersions("1.0", "1.0.0") == 0);
    CHECK(updater::CompareVersions("0.1.0", "v0.1.0") == 0);
    CHECK(updater::CompareVersions("0.1.0-rc1", "0.1.0") == 0);
    CHECK(updater::CompareVersions("0.1.0", "0.1.1") < 0);
}

TEST(updater_json_string)
{
    const std::string json = R"({"url":"x","author":{"login":"someone"},"tag_name": "v0.2.0","name":"First \"public\" build","draft":false,
        "assets":[{"name":"SCDA_Setup.exe"}]})";
    CHECK_EQ(updater::JsonString(json, "tag_name"), std::string("v0.2.0"));
    CHECK_EQ(updater::JsonString(json, "name"), std::string("First \"public\" build"));
    CHECK_EQ(updater::JsonString(json, "body"), std::string());
    CHECK_EQ(updater::JsonString(json, "draft"), std::string()); // not a string
}
