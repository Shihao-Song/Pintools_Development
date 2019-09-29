#ifndef __BP_CONSTANTS_HH__
#define __BP_CONSTANTS_HH__

namespace BP
{
class CONSTANTS
{
  public:
    static const unsigned instShiftAmt = 2; // Number of bits to shift a PC by

    // You can play around with these settings.
    static const unsigned localPredictorSize = 4096;
    static const unsigned localCounterBits = 2;
    static const unsigned localHistoryTableSize = 4096;
    static const unsigned globalPredictorSize = 8192;
    static const unsigned globalCounterBits = 2;
    static const unsigned choicePredictorSize = 8192; // Keep this the same as globalPredictorSize.
    static const unsigned choiceCounterBits = 2;
};
}

#endif
