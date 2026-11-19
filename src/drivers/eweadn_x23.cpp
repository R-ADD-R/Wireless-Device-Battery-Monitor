// EWEADN X23 Pro with the YDX 2.4 GHz receiver (04F3:026F).
// The official web driver (https://hub.eweadn.cn/ydx/web/index.html) reads
// feature report 05 and gets the battery from input report 04. GamingMS.exe
// checks the "1010" signature and the link flag in feature report 05.
#include "drivers.h"

#include <algorithm>
#include <cwchar>
#include <map>

namespace {

constexpr USHORT kVendor = 0x04F3;
constexpr USHORT kReceiver = 0x026F;

struct Receiver {
    HidDev device;
    int percent = -1;
    bool charging = false;
};

// Keep the input handle open so Windows can queue updates between polls.
std::map<std::wstring, Receiver> g_receivers;

bool IsStatusCollection(const HidColl& c) {
    return c.iface == 1 && c.page() == 0xFF00 && c.usage() == 1 && c.inLen() >= 10 && c.featLen() >= 32;
}

int ReadLink(HidDev& device) {
    std::vector<BYTE> status;
    if (!device.GetFeature(0x05, status) || status.size() < 6 || status[0] != 0x05 ||
        status[1] != '1' || status[2] != '0' || status[3] != '1' || status[4] != '0' || status[5] > 1)
        return -1;
    return status[5];
}

bool ReadUpdates(Receiver& receiver) {
    const ULONGLONG deadline = GetTickCount64() + 1000;
    bool updated = false;
    std::vector<BYTE> report;
    for (int count = 0; count < 64; ++count) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const int remaining = static_cast<int>(deadline - now);
        const int timeout = receiver.percent >= 0 ? std::min(25, remaining) : remaining;
        if (receiver.device.Read(report, timeout) < 0) break;
        // The Win32 buffer has the report ID before the WebHID payload.
        if (report.size() < 10 || report[0] != 0x04 || report[4] > 100 || report[5] > 2) continue;
        if (report[1] == 0 && report[2] == 0 && report[3] == 0 && report[4] == 0 && report[5] == 0) continue;
        receiver.percent = report[5] == 2 ? 100 : report[4];
        receiver.charging = report[5] == 1;
        updated = true;
    }
    return updated;
}

}  // namespace

void PollEweadnX23(PollContext& ctx) {
    std::set<std::wstring> present;
    if (!ctx.IsClaimed(kVendor, kReceiver)) {
        for (const HidColl* c : ctx.Find(kVendor, kReceiver, IsStatusCollection)) {
            auto it = g_receivers.find(c->path);
            if (it == g_receivers.end()) {
                // Other brands share this VID/PID. The official X23 driver identifies this
                // receiver by the USB product string as well as its numeric IDs.
                if (_wcsicmp(HidProduct(*c).c_str(), L"X23 Pro Receiver") != 0) continue;
                it = g_receivers.try_emplace(c->path).first;
                if (!it->second.device.OpenReadOnly(*c)) {
                    g_receivers.erase(it);
                    continue;
                }
            }
            present.insert(c->path);
            Receiver& receiver = it->second;
            const int link = ReadLink(receiver.device);
            if (link == 0) {
                receiver.percent = -1;
                receiver.device.Flush();
                ctx.Claim(kVendor, kReceiver);
                continue;
            }
            bool updated = ReadUpdates(receiver);
            if (link < 0 && !updated) {
                g_receivers.erase(it);  // reconnect on the next poll if the handle stopped responding
                continue;
            }
            if (receiver.percent < 0) continue;

            DeviceStatus status;
            status.key = L"eweadn-x23:" + c->path;
            status.name = L"EWEADN X23 Pro";
            status.via = L"2.4 GHz receiver";
            status.kind = DeviceKind::Mouse;
            status.percent = receiver.percent;
            status.charging = receiver.charging;
            ctx.Add(status, kVendor, kReceiver);
        }
    }
    for (auto it = g_receivers.begin(); it != g_receivers.end();) {
        if (present.count(it->first)) ++it;
        else it = g_receivers.erase(it);
    }
}
