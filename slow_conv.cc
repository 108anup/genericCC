#include "slow_conv.hh"
#include <sstream>

SeqNumDelta SlowConv::count_loss(SeqNum seq) {
	SeqNumDelta segs_lost = 0;
	for (auto it = unacknowledged_segs.begin(); it != unacknowledged_segs.end();
		 it++) {
		if (it->first < seq && !it->second.lost) {
			segs_lost++;
			it->second.lost = true;
		}
	}
	return segs_lost;
}

void SlowConv::onACK(SeqNum ack, Time receiver_timestamp __attribute((unused)),
					 Time sender_timestamp __attribute((unused))) {
	// std::cout << "onACK: " << ack << "\n";
	SeqNum seq = ack - 1;

	if (unacknowledged_segs.count(seq) == 0) {
		log(LogLevel::ERROR, "Unknown Ack!! " + std::to_string(seq));
		return;
	}
	if (unacknowledged_segs.count(seq) > 1) {
		log(LogLevel::ERROR, "Dupsent!! " + std::to_string(seq));
		return;
	}
	assert(unacknowledged_segs.count(seq) == 1);

	SegmentData& seg = unacknowledged_segs[seq];
	Time sent_time = seg.send_tstamp;
	unacknowledged_segs.erase(seq);
	if(seg.lost) {
		log(LogLevel::ERROR, "ACK for lost pkt. There is pkt reordering " + std::to_string(seq));
		cum_segs_lost--;
	}

	SeqNumDelta segs_lost = count_loss(seq);
	seg.this_loss_count = segs_lost;

	Time now = current_timestamp();
	seg.rtt = std::max((TimeDelta)0, now - sent_time);

	cum_segs_delivered++;
	cum_segs_lost+=segs_lost;

	update_history(now, seg);  // this calls update beliefs
	update_rate_cwnd(now);
}

void SlowConv::onPktSent(SeqNum seq) {
	// std::cout << "onPktSent: " << seq << "\n";

	Time now = current_timestamp();
	// TODO: consider using multimap, as this will overwrite existing entry.
	if (unacknowledged_segs.count(seq) != 0) {
		log(LogLevel::ERROR, "Dupsent!! " + std::to_string(seq));
	} else {
		unacknowledged_segs[seq] = {now, cum_segs_delivered, 0, 0, false};
		cum_segs_sent++;
	}
	// TODO: Separately maintain sending history.
	update_rate_cwnd(now);
}

void SlowConv::update_beliefs_minc_maxc(Time now, SegmentData seg __attribute((unused))) {
	// std::cout << "update_beliefs_minc_maxc " << "\n";
	if (history.size() <= 1) {
		return;
	}

	TimeDelta rtprop = beliefs.min_rtt;
	TimeDelta jitter = beliefs.min_rtt * JITTER_MULTIPLIER;

	bool this_high_delay;
	bool this_loss;
	bool this_utilized;
	bool cum_utilized = true;

	SegsRate fresh_minc = INIT_MIN_C;
	SegsRate fresh_maxc = INIT_MAX_C;

	const History &et = history.back();
	size_t depth = 0;
	auto hit = history.rbegin();
	hit++;
	for (; hit != history.rend(); hit++) {
		depth++;
		const History &st = *hit;

		this_high_delay = st.interval_min_rtt > rtprop + jitter;
		this_loss = st.interval_segs_lost > 0;
		this_utilized = this_high_delay || this_loss;
		cum_utilized = cum_utilized && this_utilized;

		TimeDelta this_time_window = et.creation_tstamp - st.creation_tstamp;
		SeqNumDelta this_delivered_segs =
			et.creation_cum_delivered_segs - st.creation_cum_delivered_segs;

		fresh_minc = std::max(fresh_minc, (this_delivered_segs * MS_TO_SECS) /
										  (this_time_window + jitter));
		TimeDelta denom = std::max((TimeDelta)0, this_time_window - jitter);
		if (cum_utilized && depth > 1 && denom > 0) {
			fresh_maxc = std::min(fresh_maxc,
								  (this_delivered_segs * MS_TO_SECS) / denom);
		}
	}

	beliefs.minc_since_last_timeout = std::max(beliefs.minc_since_last_timeout, fresh_minc);
	beliefs.maxc_since_last_timeout = std::min(beliefs.maxc_since_last_timeout, fresh_maxc);
	beliefs.maxc_since_last_timeout = std::max(beliefs.maxc_since_last_timeout, get_min_sending_rate());
	beliefs.min_c = std::max(beliefs.min_c, fresh_minc);
	beliefs.max_c = std::min(beliefs.max_c, fresh_maxc);
	beliefs.max_c = std::max(beliefs.max_c, get_min_sending_rate());

	/**
	 * There are 4 things:
	 * - minc since start (using all intervals till now). also known as overall (deprecated)
	 * - minc fresh (just using intervals that end now)
	 * - minc since last timeout (using all intervals since last timeout). also
	 * 	 known as recomputed
	 * - minc at last timeout (use all intervals before last
	 *   timeout)
	 */

	TimeDelta time_since_last_timeout = now - last_timeout_time;
	bool timeout = time_since_last_timeout > beliefs.min_rtt * BELIEFS_TIMEOUT_PERIOD;

	if (timeout) {
		// std::cout << "timeout " << "\n";
		last_timeout_time = now;
		bool minc_changed = beliefs.min_c > beliefs.last_timeout_minc;
		bool maxc_changed = beliefs.max_c < beliefs.last_timeout_maxc;

		bool minc_changed_significantly =
			beliefs.min_c >
			BELIEFS_CHANGED_SIGNIFICANTLY_THRESH * beliefs.last_timeout_minc;
		bool maxc_changed_significantly =
			beliefs.max_c * BELIEFS_CHANGED_SIGNIFICANTLY_THRESH <
			beliefs.last_timeout_maxc;
		bool beliefs_invalid = beliefs.max_c < beliefs.min_c;
		bool minc_came_close = minc_changed && beliefs_invalid;
		bool maxc_came_close = maxc_changed && beliefs_invalid;
		bool timeout_minc =
			!minc_changed && (maxc_came_close || !maxc_changed_significantly);
		bool timeout_maxc =
			!maxc_changed && (minc_came_close || !minc_changed_significantly);

		if (timeout_minc) {
			beliefs.min_c = beliefs.minc_since_last_timeout;
		}

		if (timeout_maxc) {
			beliefs.max_c = std::min(beliefs.max_c * TIMEOUT_THRESH,
									 beliefs.maxc_since_last_timeout);
		}

		beliefs.last_timeout_maxc = beliefs.max_c;
		beliefs.last_timeout_minc = beliefs.min_c;
		beliefs.minc_since_last_timeout = INIT_MIN_C;
		beliefs.maxc_since_last_timeout = INIT_MAX_C;
	}
}

SegsRate SlowConv::get_min_sending_rate() {
	return (MIN_CWND * MS_TO_SECS) / beliefs.min_rtt;
}

void SlowConv::update_beliefs_minc_lambda(Time now __attribute((unused)), SegmentData seg) {
	// std::cout << "update_beliefs_minc_lambda" << std::endl;
	if (history.size() <= 1) {
		return;
	}

	TimeDelta rtprop = beliefs.min_rtt;
	TimeDelta jitter = beliefs.min_rtt * JITTER_MULTIPLIER;

	bool this_high_delay;
	bool this_loss;
	bool this_underutilized;
	bool cum_underutilized;

	const History &latest = history.back();
	this_high_delay = latest.interval_max_rtt > rtprop + jitter;
	this_loss = latest.interval_segs_lost > 0;
	this_underutilized = !this_high_delay && !this_loss;
	cum_underutilized = this_underutilized;

	SeqNum delivered_1rtt_ago = seg.cum_delivered_segs_at_send;
	// latest.creation_cum_delivered_segs_at_send;

	SegsRate new_minc_lambda = INIT_MIN_C;
	size_t depth = 0;
	// TODO: can start from history.size()-1.
	auto hit = history.rbegin();
	hit++;
	int hid = history.size()-1;
	for (; hit != history.rend(); hit++) {
		hid--;
		depth++;
		History &st = *hit;

		this_high_delay = st.interval_max_rtt > rtprop + jitter;
		this_loss = st.interval_segs_lost > 0;
		this_underutilized = !this_high_delay && !this_loss;
		cum_underutilized = cum_underutilized && this_underutilized;

		if (depth < MEASUREMENT_INTERVAL_HISTORY) continue;

		const History &et = history[hid + MEASUREMENT_INTERVAL_HISTORY];

		if (et.creation_cum_delivered_segs > delivered_1rtt_ago)
			continue;

		st.processed = true;

		if (!cum_underutilized) break;

		SeqNumDelta this_segs_sent =
			et.creation_cum_sent_segs - st.creation_cum_sent_segs;
		TimeDelta this_time_window = et.creation_tstamp - st.creation_tstamp;
		new_minc_lambda = std::max(
			new_minc_lambda, (this_segs_sent * MS_TO_SECS) / this_time_window);
	}

	beliefs.min_c_lambda = std::max(beliefs.min_c_lambda, new_minc_lambda);
	// TODO: Implement timeout
}

void SlowConv::update_beliefs(Time now, SegmentData seg, bool updated_history,
							  TimeDelta time_since_last_update) {
	// std::cout << "update_beliefs" << std::endl;
	beliefs.min_rtt = std::min(beliefs.min_rtt, seg.rtt);
	TimeDelta jitter = beliefs.min_rtt * JITTER_MULTIPLIER;

	beliefs.min_qdel =
		std::max((TimeDelta)0, seg.rtt - beliefs.min_rtt - jitter);
	beliefs.bq_belief1 = cum_segs_sent - cum_segs_delivered - cum_segs_lost;

	if (updated_history) {
		SeqNumDelta estimated_sent = cum_segs_sent - beliefs.last_segs_sent;
		SeqNumDelta estimated_delivered =
			beliefs.min_c_lambda * time_since_last_update;
		beliefs.bq_belief2 =
			beliefs.bq_belief2 + estimated_sent - estimated_delivered;
		beliefs.bq_belief2 = std::max((SeqNumDelta)0, beliefs.bq_belief2);
		beliefs.bq_belief2 = std::min(beliefs.bq_belief2, beliefs.bq_belief1);
		beliefs.last_segs_sent = cum_segs_sent;

		update_beliefs_minc_maxc(now, seg);
		update_beliefs_minc_lambda(now, seg);
	}
}

void SlowConv::update_history(Time now, SegmentData seg) {
	// std::cout << "update_history" << std::endl;
	TimeDelta inter_history_time = INTER_HISTORY_TIME * beliefs.min_rtt;
	TimeDelta time_since_last_update = now - last_history_update_time;
	if (time_since_last_update >= inter_history_time) {
		last_history_update_time = now;
		History h = {now,
					 seg.rtt,
					 seg.rtt,
					 cum_segs_sent,
					 cum_segs_delivered,
					 cum_segs_lost,
					 sending_rate,
					 seg.cum_delivered_segs_at_send,
					 seg.this_loss_count,
					 false};
		history.push_back(h);
		update_beliefs(now, seg, true, time_since_last_update);
	} else {
		History &latest = history.back();
		latest.interval_max_rtt = std::max(latest.interval_max_rtt, seg.rtt);
		latest.interval_min_rtt = std::min(latest.interval_min_rtt, seg.rtt);
		update_beliefs(now, seg, false, time_since_last_update);
	}
}

void SlowConv::update_rate_cwnd(Time now) {
	// std::cout << "update_rate_cwnd" << std::endl;
	TimeDelta time_since_last_rate_update = now - last_rate_update_time;
	if (time_since_last_rate_update >= INTER_RATE_UPDATE_TIME * beliefs.min_rtt) {
		last_rate_update_time = now;

		if(state == State::SLOW_START) {
			update_rate_cwnd_fast_conv(now);
		}
		else if (state == State::CONG_AVOID) {
			update_rate_cwnd_slow_conv(now);
		}
		else {
			log(LogLevel::ERROR,
				"State not implemented: " + std::to_string(state));
			// assert(false);
		}
		log_state(now);
		log_beliefs(now);
		log_history(now);
	}
}

void SlowConv::update_rate_cwnd_slow_conv(Time now __attribute((unused))) {
	TimeDelta rtprop = beliefs.min_rtt;
	TimeDelta jitter = beliefs.min_rtt * JITTER_MULTIPLIER;

	SegsRate min_sending_rate = get_min_sending_rate();
	if(beliefs.bq_belief1 > 2 * MIN_CWND) {
		sending_rate = min_sending_rate;
	} else {
		sending_rate =
			(1 + JITTER_MULTIPLIER) * beliefs.min_c_lambda + min_sending_rate;
	}
	sending_rate = std::max(sending_rate, min_sending_rate);
	cwnd = (2 * beliefs.max_c * (rtprop + jitter)) / MS_TO_SECS;

	_intersend_time = MS_TO_SECS / sending_rate;
	_the_window = cwnd;
}

void SlowConv::update_rate_cwnd_fast_conv(Time now __attribute((unused))) {
	TimeDelta rtprop = beliefs.min_rtt;
	TimeDelta jitter = beliefs.min_rtt * JITTER_MULTIPLIER;

	SegsRate min_sending_rate = get_min_sending_rate();
	if(beliefs.min_qdel > 0) {
		sending_rate = beliefs.min_c / 2;
	} else {
		sending_rate = (1 + JITTER_MULTIPLIER) * beliefs.min_c;
	}
	sending_rate = std::max(sending_rate, min_sending_rate);
	cwnd = (2 * beliefs.max_c * (rtprop + jitter)) / MS_TO_SECS;

	_intersend_time = MS_TO_SECS / sending_rate;
	_the_window = cwnd;
}

void SlowConv::log(LogLevel l, std::string msg) {
	if (logfile.is_open()) {
		// std::cout << "Logging";
		logfile << LOG_TYPE_TO_STR[l] << " " << msg << std::endl;
	} else {
		// std::cout << "Log file closed.";
	}
}

void SlowConv::log_state(Time now) {
	std::stringstream ss;
	ss << "time " << now << " cwnd " << cwnd << " sending_rate " << sending_rate
	   << " state " << state;
	ss << " cum_segs_sent " << cum_segs_sent << " cum_segs_delivered "
	   << cum_segs_delivered << " cum_segs_lost " << cum_segs_lost;
	log(LogLevel::INFO, ss.str());
}

void SlowConv::log_beliefs(Time now) {
	std::stringstream ss;
	ss << "time " << now << " min_rtt " << beliefs.min_rtt << " min_qdel "
	   << beliefs.min_qdel << " min_c " << beliefs.min_c << " max_c "
	   << beliefs.max_c;
	ss << " min_c_lambda " << beliefs.min_c_lambda << " bq_belief1 "
	   << beliefs.bq_belief1 << " bq_belief2 " << beliefs.bq_belief2;
	log(LogLevel::INFO, ss.str());
}

void SlowConv::log_history(Time now __attribute((unused))) {
	for (auto h = history.rbegin(); h != history.rend(); ++h) {
		// std::stringstream ss;
		// ss << "time " << now;
		// ss << h.to_string();
		log(LogLevel::DEBUG, h->to_string());
	}
}

void SlowConv::init() {
	// Time now = current_timestamp();
	genericcc_min_rtt = 0;
	genericcc_rate_measurement = 0;

	last_timeout_time = 0;
	last_rate_update_time = 0;
	last_history_update_time = 0;
	state = State::SLOW_START;

	unacknowledged_segs.clear();
	history.clear();
	beliefs = Beliefs();

	cum_segs_sent = 0;
	cum_segs_delivered = 0;
	cum_segs_lost = 0;
	sending_rate = INIT_MIN_C;
	cwnd = MIN_CWND;

	_intersend_time = MS_TO_SECS / sending_rate;
	_the_window = cwnd;
}