#ifndef FASTCONV_HH
#define FASTCONV_HH

#include "slow_conv.hh"

class FastConv : public SlowConv {
    // https://stackoverflow.com/questions/347358/inheriting-constructors
    using SlowConv::SlowConv; // inherit constructor

    void update_state(Time __attribute((unused)), SegmentData __attribute((unused))) override;
};

#endif