#ifndef SLOWCONV_HH
#define SLOWCONV_HH

#include <boost/circular_buffer.hpp>
#include <sstream>
#include <fstream>
#include <map>

#include "ccc.hh"

typedef double Time;		  // ms (cumulative)
typedef double TimeDelta;	  // ms
typedef int32_t SeqNum;		  // Segments (Segs)
typedef int32_t SeqNumDelta;  // Segs (cuumulative)
typedef double SegsRate;	  // Segs per second

class SlowConv: public CCC {
	// TODO: have a separate class for belief based CCA.
	//  Just keep the update cwnd/rate logic in the derived classes...
   public:
	struct History {
		Time creation_tstamp;

		TimeDelta interval_min_rtt;
		TimeDelta interval_max_rtt;

		SeqNum creation_cum_sent_segs;
		SeqNum creation_cum_delivered_segs;
		SeqNum creation_cum_lost_segs;

		SegsRate creation_sending_rate;

		// What was delivered when the packet corresponding to this ACK was
		// sent.
		SeqNum creation_cum_delivered_segs_at_send;

		SeqNumDelta interval_segs_lost;

		bool processed;

		std::string to_string() {
			std::stringstream ss;
			ss << "creation_tstamp " << creation_tstamp << " interval_min_rtt "
			   << interval_min_rtt << " interval_max_rtt " << interval_max_rtt
			   << " creation_cum_sent_segs " << creation_cum_sent_segs
			   << " creation_cum_delivered_segs " << creation_cum_delivered_segs
			   << " creation_cum_lost_segs " << creation_cum_lost_segs
			   << " creation_sending_rate " << creation_sending_rate
			   << " creation_cum_delivered_segs_at_send "
			   << creation_cum_delivered_segs_at_send << " interval_segs_lost "
			   << interval_segs_lost << " processed " << processed;
			return ss.str();
		}
	};

	struct Beliefs {
		// Updated on every ACK
		TimeDelta min_rtt;
		TimeDelta min_qdel;
		SeqNumDelta bq_belief1;	 // inflight segs

		// Updated every rtprop time (on seg sent)
		SegsRate min_c_lambda;
		SegsRate last_min_c_lambda;
		SeqNumDelta bq_belief2;	 // estimated bottleneck queue segs
		SeqNum last_segs_sent;

        SegsRate min_c;
        SegsRate max_c;

		SegsRate minc_since_last_timeout;
        SegsRate maxc_since_last_timeout;

		SegsRate last_timeout_minc;
        SegsRate last_timeout_maxc;

		Beliefs()
			: min_rtt(TIME_DELTA_MAX),
			  min_qdel(TIME_DELTA_MAX),
			  bq_belief1(0),
			  min_c_lambda(INIT_MIN_C),
			  last_min_c_lambda(INIT_MIN_C),
			  bq_belief2(0),
			  last_segs_sent(0),
			  min_c(INIT_MIN_C),
			  max_c(INIT_MAX_C),
			  minc_since_last_timeout(INIT_MIN_C),
			  maxc_since_last_timeout(INIT_MAX_C),
			  last_timeout_minc(INIT_MIN_C),
			  last_timeout_maxc(INIT_MAX_C) {}
	};

	enum State { SLOW_START, CONG_AVOID, PROBE, DRAIN };

	enum LogLevel { ERROR, INFO, DEBUG };

	struct SegmentData {
		// On send
		Time send_tstamp;
		SeqNum cum_delivered_segs_at_send;

		// On ACK
		TimeDelta rtt;
		SeqNumDelta this_loss_count;
		bool lost;
	};

	static constexpr int HISTORY_SIZE = 32;
	static constexpr SeqNumDelta MIN_CWND = 5;
	static constexpr SegsRate INIT_MIN_C = MIN_CWND;  // ~60 Kbps
	static constexpr SegsRate INIT_MAX_C = 1e5;		  // ~1.2 Gbps
	static constexpr TimeDelta TIME_DELTA_MAX = 1e5;  // 1e2 seconds
	static constexpr TimeDelta MS_TO_SECS = 1e3;
	static constexpr double INTER_HISTORY_TIME = 1;		 // Multiple of min_rtt
	static constexpr double BELIEFS_TIMEOUT_PERIOD = 12;	 // Multiple of min_rtt
	static constexpr double JITTER_MULTIPLIER = 1;		 // Multiple of min_rtt
	static constexpr int MEASUREMENT_INTERVAL_RTPROP = 1;	 // Multiple of min_rtt
	static constexpr int MEASUREMENT_INTERVAL_HISTORY =
		MEASUREMENT_INTERVAL_RTPROP / INTER_HISTORY_TIME;	// Multiple of history
	static constexpr double BELIEFS_CHANGED_SIGNIFICANTLY_THRESH = 1.1;
	static constexpr double TIMEOUT_THRESH = 1.5;
    static constexpr int INTER_RATE_UPDATE_TIME = 1; // Multiple of min_rtt
	std::string LOG_TYPE_TO_STR[3];

   protected:
	Time cur_tick;

	Time genericcc_min_rtt;
	double genericcc_rate_measurement;

	Time last_timeout_time;
	Time last_rate_update_time;
	Time last_history_update_time;
	State state;

	std::map<SeqNum, SegmentData> unacknowledged_segs;
	boost::circular_buffer<History> history;
	Beliefs beliefs;

	SeqNum cum_segs_sent;
	SeqNum cum_segs_delivered;
	SeqNum cum_segs_lost;
	SegsRate sending_rate;
    SeqNumDelta cwnd;

	std::string logfilepath;
	std::ofstream logfile;

	Time current_timestamp() { return cur_tick; }
	SegsRate get_min_sending_rate();
	void update_beliefs_minc_maxc(Time, SegmentData __attribute((unused)));
	void update_beliefs_minc_lambda(Time __attribute((unused)), SegmentData);
	void update_beliefs(Time, SegmentData, bool, TimeDelta);
	void update_history(Time, SegmentData);
	void update_rate_cwnd(Time);
	void update_rate_cwnd_fast_conv(Time __attribute((unused)));
	void update_rate_cwnd_slow_conv(Time __attribute((unused)));
	virtual void update_state(Time __attribute((unused)), SegmentData);
	void log(LogLevel, std::string);
	void log_state(Time);
	void log_beliefs(Time);
	void log_history(Time __attribute((unused)));
	SeqNumDelta count_loss(SeqNum seq);

   public:
	SlowConv(std::string logfilepath = "")
		:
		  LOG_TYPE_TO_STR({"ERROR", "INFO", "DEBUG"}),
		  cur_tick(0),
		  genericcc_min_rtt(0),
		  genericcc_rate_measurement(0),
		  last_timeout_time(0),
		  last_rate_update_time(0),
		  last_history_update_time(0),
		  state(State::SLOW_START),
		  unacknowledged_segs(),
		  history(HISTORY_SIZE),
		  beliefs(Beliefs()),
		  cum_segs_sent(0),
		  cum_segs_delivered(0),
		  cum_segs_lost(0),
		  sending_rate(INIT_MIN_C),
		  cwnd(MIN_CWND),
		  logfilepath(logfilepath),
		  logfile()
	{
		if (!logfilepath.empty()) {
			std::cout << "Logging at " << logfilepath << "\n";
			logfile.open(logfilepath);
		}
	}

	~SlowConv() {
		if (logfile.is_open()) logfile.close();
	}

	virtual void init() override;
	virtual void onACK(SeqNum ack, Time receiver_timestamp __attribute((unused)),
					   Time sender_timestamp __attribute((unused))) override;
	virtual void onPktSent(SeqNum seq_num) override;
	virtual void onTimeout() override { std::cerr << "Ack timed out!\n"; }
	virtual void onLinkRateMeasurement(double s_measured_link_rate) override {
		genericcc_rate_measurement = s_measured_link_rate;
	}
	void set_timestamp(Time s_cur_tick) { cur_tick = s_cur_tick; }
	void set_min_rtt(Time s_min_rtt) { genericcc_min_rtt = s_min_rtt; }
};

#endif