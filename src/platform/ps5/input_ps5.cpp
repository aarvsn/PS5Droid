// PS5Droid - PS5 pad backend (scePad over the DualSense pad).
//
// STATUS: implemented against verified API usage (sce_stubs symbols +
// ps5-payload-dev/SDL button layout and struct), never executed on a
// console. See docs/platform-constraints.md.
#include <cstring>

#include "platform/display.h"
#include "platform/ps5/sce_decls.h"

namespace ps5droid {
namespace platform {
namespace {

class Ps5Input : public InputBackend {
  public:
    bool init() override {
        if (inited_)
            return true;
        if (ps5::scePadInit() != 0)
            return false;
        int user_id = ps5::kUserIdSystem;
        int initial_user = -1;
        if (ps5::sceUserServiceGetInitialUser(&initial_user) == 0 && initial_user >= 0) {
            user_id = initial_user;
        }

        handle_ = ps5::scePadOpen(user_id, ps5::kPadPortTypeStandard, 0, nullptr);
        if (handle_ < 0 && user_id != ps5::kUserIdSystem) {
            handle_ = ps5::scePadOpen(ps5::kUserIdSystem, ps5::kPadPortTypeStandard, 0, nullptr);
        }
        if (handle_ < 0) {
            handle_ = -1;
            return false;
        }
        inited_ = true;
        return true;
    }

    bool poll(PadState* out) override {
        if (!inited_ || out == nullptr)
            return false;
        ps5::ScePadData data;
        memset(&data, 0, sizeof(data));
        if (ps5::scePadReadState(handle_, &data) != 0)
            return false;

        out->connected = data.connected != 0;
        out->up = (data.buttons & ps5::kPadButtonUp) != 0;
        out->down = (data.buttons & ps5::kPadButtonDown) != 0;
        out->left = (data.buttons & ps5::kPadButtonLeft) != 0;
        out->right = (data.buttons & ps5::kPadButtonRight) != 0;
        out->cross = (data.buttons & ps5::kPadButtonCross) != 0;
        out->circle = (data.buttons & ps5::kPadButtonCircle) != 0;
        out->square = (data.buttons & ps5::kPadButtonSquare) != 0;
        out->triangle = (data.buttons & ps5::kPadButtonTriangle) != 0;
        out->options = (data.buttons & ps5::kPadButtonOptions) != 0;
        out->l1 = (data.buttons & ps5::kPadButtonL1) != 0;
        out->r1 = (data.buttons & ps5::kPadButtonR1) != 0;
        out->l2 = (data.buttons & ps5::kPadButtonL2) != 0;
        out->r2 = (data.buttons & ps5::kPadButtonR2) != 0;
        out->l3 = (data.buttons & ps5::kPadButtonL3) != 0;
        out->r3 = (data.buttons & ps5::kPadButtonR3) != 0;
        out->touchpad = (data.buttons & ps5::kPadButtonTouchpad) != 0;
        out->lx = data.left_stick.x;
        out->ly = data.left_stick.y;
        out->rx = data.right_stick.x;
        out->ry = data.right_stick.y;
        out->l2_value = data.analog_buttons.l2;
        out->r2_value = data.analog_buttons.r2;
        return true;
    }

    void shutdown() override {
        if (handle_ >= 0) {
            ps5::scePadClose(handle_);
            handle_ = -1;
        }
        inited_ = false;
    }

    const char* name() const override { return "ps5-pad"; }

  private:
    int handle_ = -1;
    bool inited_ = false;
};

} // namespace

InputBackend* create_ps5_input() {
    return new Ps5Input();
}

} // namespace platform
} // namespace ps5droid
