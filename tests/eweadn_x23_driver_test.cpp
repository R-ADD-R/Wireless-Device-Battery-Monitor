#include "../src/drivers/drivers.h"

#include <cassert>
#include <deque>
#include <map>

namespace {

struct SimulatedReceiver {
    std::wstring product = L"X23 Pro Receiver";
    std::vector<BYTE> link = { 0x05, '1', '0', '1', '0', 1 };
    std::deque<std::vector<BYTE>> input;
    int opens = 0;
};

std::map<std::wstring, SimulatedReceiver> receivers;

HidColl Collection(const wchar_t* path) {
    HidColl c;
    c.path = path;
    c.vid = 0x04F3;
    c.pid = 0x026F;
    c.iface = 1;
    c.caps.UsagePage = 0xFF00;
    c.caps.Usage = 1;
    c.caps.InputReportByteLength = 32;
    c.caps.FeatureReportByteLength = 32;
    return c;
}

std::vector<BYTE> BatteryReport(BYTE percent, BYTE charging = 0) {
    return { 0x04, 1, 2, 1, percent, charging, 1, 0, 1, 0 };
}

}  // namespace

bool HidDev::OpenReadOnly(const HidColl& c) {
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    ++receivers.at(c.path).opens;
    return true;
}

bool HidDev::OpenFeatureOnly(const HidColl&) { assert(false); return false; }
bool HidDev::SetFeature(const std::vector<BYTE>&) { assert(false); return false; }
bool HidDev::GetFeature(BYTE, std::vector<BYTE>&) { assert(false); return false; }

void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }

bool HidDev::GetFeatureTimed(BYTE id, std::vector<BYTE>& out, DWORD timeout) {
    assert(id == 0x05 && timeout == 200);
    out = receivers.at(c_.path).link;
    return !out.empty();
}

int HidDev::Read(std::vector<BYTE>& out, int timeout) {
    assert(timeout > 0 && timeout <= 1000);
    auto& input = receivers.at(c_.path).input;
    if (input.empty()) return -1;
    out = std::move(input.front());
    input.pop_front();
    return static_cast<int>(out.size());
}

void HidDev::Flush() { receivers.at(c_.path).input.clear(); }
std::wstring HidProduct(const HidColl& c) { return receivers.at(c.path).product; }

int main() {
    auto target = Collection(L"target");
    auto other = Collection(L"other");
    auto settings = Collection(L"settings");
    settings.caps.Usage = 2;
    receivers[target.path] = {};
    receivers[other.path].product = L"Another receiver";

    auto poll = [&] {
        PollContext ctx;
        ctx.colls = { other, settings, target };
        PollEweadn(ctx);
        return ctx.out;
    };

    auto& receiver = receivers.at(target.path);
    receiver.input = { { 0x04, 0 }, BatteryReport(101), BatteryReport(90) };
    auto result = poll();
    assert(result.size() == 1 && result[0].percent == 90 && !result[0].charging);
    assert(result[0].name == L"EWEADN X23 Pro" && result[0].kind == DeviceKind::Mouse);
    assert(receivers.at(other.path).opens == 0 && receiver.opens == 1);
    const auto key = result[0].key;

    result = poll();  // the mouse is still linked even without a new input report
    assert(result.size() == 1 && result[0].percent == 90 && receiver.opens == 1);
    receiver.input = { BatteryReport(91), BatteryReport(92, 1) };
    result = poll();
    assert(result.size() == 1 && result[0].percent == 92 && result[0].charging);

    receiver.link[5] = 0;
    receiver.input = { BatteryReport(92) };
    assert(poll().empty());
    receiver.link[5] = 1;
    assert(poll().empty());  // a reconnect must supply its own reading
    receiver.input = { BatteryReport(85, 2) };
    result = poll();
    assert(result.size() == 1 && result[0].percent == 100 && !result[0].charging && result[0].full);

    PollContext ignored;
    ignored.colls = { target };
    ignored.Claim(0x04F3, 0x026F);
    PollEweadn(ignored);
    assert(ignored.out.empty());
    receiver.input = { BatteryReport(0) };
    result = poll();
    assert(result.size() == 1 && result[0].percent == 0 && receiver.opens == 2);

    target.path = L"new-port";
    receivers[target.path].input = { BatteryReport(20) };
    result = poll();
    assert(result.size() == 1 && result[0].key == key && result[0].percent == 20);
}
