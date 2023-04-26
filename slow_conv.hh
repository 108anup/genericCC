#include <unordered_map>
#include <boost/circular_buffer.hpp>

#include "ccc.hh"

typedef double Time;  // ms (cumulative)
typedef double TimeDelta;  // ms
typedef int32_t SeqNum;  // Segments (Segs)
typedef int32_t SeqNumDelta;  // Segs (cuumulative)
typedef double SegsRate;  // Segs per second

class SlowConv : public CCC {
   public:
	struct History {
		Time creation_tstamp;

        TimeDelta interval_min_rtt;
		TimeDelta interval_max_rtt;

		SeqNum creation_sent_segs;
        SeqNum creation_delivered_segs;
		SeqNum creation_lost_segs;

		SegsRate creation_sending_rate;

        // What was delivered when the packet corresponding to this ACK was sent.
		SeqNum creation_cum_segs_delivered_at_send;
	};

	struct Beliefs {
        // Updated on every ACK
        TimeDelta min_rtt;
		TimeDelta min_qdel;
		SeqNumDelta bq_belief1;  // inflight segs

        // Updated every rtprop time (on seg sent)
        SegsRate min_c_lambda;
		SegsRate last_min_c_lambda;
        SeqNumDelta bq_belief2;  // estimated bottleneck queue segs
        SeqNum last_segs_sent;
	};

	enum State {
		SLOW_START,
		PROBE,
		DRAIN
	};

	struct SegmentData {
		Time send_tstamp;
		SeqNum cum_segs_delivered_at_send;
	};

	static const int HISTORY_SIZE = 32;
	static const SeqNumDelta MIN_CWND = 5;
	static const SegsRate INIT_MIN_C = MIN_CWND;  // ~60 Kbps
    static const SegsRate INIT_MAX_C = 1e5;  // ~1.2 Gbps
	static const TimeDelta MS_TO_SECS = 1e3;
	static const double INTER_HISTORY_TIME = 1;	 // Multiple of min_rtt
    static const double BELIEFS_TIMEOUT_PERIOD = 1;  // Multiple of min_rtt
    static const double JITTER_MULTIPLIER = 1; // Multiple of min_rtt

   protected:
	Time cur_tick;

	Time genericcc_min_rtt;
	double genericcc_rate_measurement;

	Time last_rate_update_time;
	Time last_history_update_time;
	State state;

	std::unordered_map<SeqNum, SegmentData> unacknowledged_segs;
	boost::circular_buffer<History> history;
	Beliefs beliefs;

    SeqNum cum_segs_sent;
    SeqNum cum_segs_delivered;
    SeqNum cum_segs_lost;
    SegsRate sending_rate;

    Time current_timestamp() { return cur_tick; }
	void update_beliefs_minc_maxc(Time now);
	void update_beliefs_minc_lambda(Time now);
	void update_beliefs(Time, SegmentData, bool, TimeDelta);
	void update_history(Time, SegmentData);
	void set_rate_cwnd();
	SeqNumDelta count_loss(SeqNum seq);

   public:
	SlowConv() {}

	virtual void init() override;
	virtual void onACK(SeqNum ack, Time receiver_timestamp,
					   Time sender_timestamp) override;
	virtual void onPktSent(SeqNum seq_num) override;
	virtual void onTimeout() override { std::cerr << "Ack timed out!\n"; }
	virtual void onLinkRateMeasurement(double s_measured_link_rate) override {
		genericcc_rate_measurement = s_measured_link_rate;
	}
	void set_timestamp(Time s_cur_tick) { cur_tick = s_cur_tick; }
	void set_min_rtt(Time s_min_rtt) { genericcc_min_rtt = s_min_rtt; }
};