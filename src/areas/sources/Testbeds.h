#pragma once

#include <QString>

#include <algorithm>
#include <iterator>

namespace reloadmaker
{
    // The automated test corpus: the testbeds the golden capture and the
    // suites open by name.
    inline constexpr const char* kTestbedIds[] = {
        "glock", "ru556", "m1a", "timberwolf", "l118a2", "thanatos",
        "srep-service", "srep-honeybadger", "srep-ar10", "srep-xm16e1",
    };

    // Whether a testbed id belongs to the corpus. Only corpus testbeds can be
    // re-pointed to another saved project (setting testbeds/<id>).
    inline bool isCorpusTestbed(const QString& id)
    {
        return std::any_of(std::begin(kTestbedIds), std::end(kTestbedIds), [&](const char* testbed) { return id == QLatin1String(testbed); });
    }

    // The dev bench: Maker's own copies of the owner's roster mods; MO2 keeps
    // its installed copies separately (owner, 2026-10-05). Every weapon
    // project saved from a source in this folder is listed beside the
    // testbeds.
    inline constexpr const char* kDevBenchFolder = "local/devbench";
}
