#include "mock_radio.h"
#define private public
#define protected public
#include "ESPNowInterface.h"
#undef private
#undef protected

int main() {
    ESPNowInterface iface;
    assert(iface.start());
    uint8_t parent[6]={2,2,2,2,2,2}, sibling[6]={3,3,3,3,3,3};
    ESPNowDiscovery discovery={}; discovery.capabilities=ESPNOW_CAP_UPSTREAM;
    iface.touch_peer(parent,&discovery); iface.touch_peer(sibling,nullptr);
    iface.pin_recovery_peer(parent,discovery,"test");
    assert(RNS::Reticulum::transport_enabled());
    // Even a host-mode node's policy must survive pin and parent expiry.
    RNS::Reticulum::transport_enabled(false);
    iface.pin_recovery_peer(parent,discovery,"test host");
    assert(!RNS::Reticulum::transport_enabled());
    tick += ESPNOW_PEER_TIMEOUT_MS+1;
    iface.loop();
    assert(!RNS::Reticulum::transport_enabled());
    RNS::Reticulum::transport_enabled(true);
    iface.touch_peer(parent,&discovery); iface.touch_peer(sibling,nullptr);
    iface.pin_recovery_peer(parent,discovery,"test");
    // Fragmented packets must reach BOTH peers even while pinned. Check the
    // actual wire emitted by the real send queue, not a reimplemented model.
    std::vector<uint8_t> payload(564,42);
    assert(iface.send_outgoing(RNS::Bytes(payload.data(),payload.size())));
    for(int i=0;i<8;++i) iface.loop();
    std::vector<uint8_t> parent_bytes,sibling_bytes;
    for(const auto& s:sent) {
        ESPNowFrameHeader h; assert(espnow_read_header(s.wire.data(),s.wire.size(),h));
        if(h.type!=ESPNOW_FRAME_DATA) continue;
        auto& out=s.mac[0]==2 ? parent_bytes : sibling_bytes;
        assert(h.fragment_index == out.size()/ESPNOW_FRAGMENT_PAYLOAD);
        out.insert(out.end(),s.wire.begin()+ESPNOW_HEADER_SIZE,s.wire.end());
    }
    assert(parent_bytes==payload && sibling_bytes==payload);
    // An unreachable parent must not consume the sibling's retry or wedge it.
    sent.clear(); fail_mac=2;
    assert(iface.send_outgoing(RNS::Bytes(payload.data(),10)));
    for(int i=0;i<8;++i) iface.loop();
    int failed=0, succeeded=0;
    for(const auto& s:sent) if(s.wire[3]==ESPNOW_FRAME_DATA) {
        if(s.mac[0]==2) ++failed;
        if(s.mac[0]==3) ++succeeded;
    }
    assert(failed==4 && succeeded==1);
    assert(iface._tx_head==iface._tx_tail);
    puts("ESP-NOW recovery, multi-peer delivery and retry tests passed");
}
