#include "slow_conv.hh"

void SlowConv::init() {
    _intersend_time = 0;
    _the_window = 2;
}

SeqNumDelta SlowConv::count_loss(SeqNum seq) {
	SeqNumDelta segs_lost = 0;
	for (auto it = unacknowledged_segs.begin(); it != unacknowledged_segs.end();
		 it++) {
		if(it->first < seq) {
            segs_lost++;
        }
	}
	return segs_lost;
}

void SlowConv::onACK(SeqNum ack, Time receiver_timestamp, Time sender_timestamp) {
    SeqNum seq = ack - 1;

    if(unacknowledged_segs.count(seq) == 0) {std::cerr<<"Unknown Ack!! "<<seq<<std::endl; return;}
    if(unacknowledged_segs.count(seq) > 1) {std::cerr<<"Dupsent!! "<<seq<<std::endl; return;}
    assert (unacknowledged_segs.count(seq) == 1);

	SegmentData seg = unacknowledged_segs[seq];
	Time sent_time = seg.send_tstamp;
	unacknowledged_segs.erase(seq);

    SeqNumDelta segs_lost = count_loss(seq);
    seg.this_loss_count = segs_lost;

    Time now = current_timestamp();
	seg.rtt = now - sent_time;

    cum_segs_lost++;
    cum_segs_delivered++;

	update_history(now, seg);  // this calls update beliefs
}

void SlowConv::onPktSent(SeqNum seq_num) {
    cum_segs_sent++;
}

void SlowConv::update_beliefs_minc_maxc(Time now) {

}

void SlowConv::update_beliefs_minc_lambda(Time now) {

    if(history.size() <= 1) {
		return;
	}

    TimeDelta rtprop = beliefs.min_rtt;
    TimeDelta jitter = beliefs.min_rtt * JITTER_MULTIPLIER;

    bool this_high_delay;
    bool this_loss;
    bool this_underutilized;
    bool cum_underutilized;

    History &latest = history.back();
	SeqNum delivered_1rtt_ago = latest.creation_cum_segs_delivered_at_send;

    this_high_delay = latest.interval_max_rtt > rtprop + jitter;
    this_loss = latest.interval_segs_lost > 0;
    this_underutilized = !this_high_delay && !this_loss;
	cum_underutilized = this_underutilized;

	SegsRate new_minc_lambda = INIT_MIN_C;
	size_t past = 0;
	for (size_t hid = history.size() - 1 - 1; hid >= 0; hid--) {
		past++;
		History &st = history[hid];

		this_high_delay = st.interval_max_rtt > rtprop + jitter;
        this_loss = st.interval_segs_lost > 0;
        this_underutilized = !this_high_delay && !this_loss;
        cum_underutilized = cum_underutilized && this_underutilized;

		if (past < MEASUREMENT_INTERVAL_HISTORY) continue;

		History &et = history[hid + MEASUREMENT_INTERVAL_HISTORY];

        if(et.creation_cum_segs_delivered_at_send > delivered_1rtt_ago)
			continue;

        st.processed = true;

        if(!cum_underutilized) break;

		SeqNumDelta this_segs_sent =
			et.creation_cum_sent_segs - st.creation_cum_sent_segs;
		TimeDelta this_time_window = et.creation_tstamp - st.creation_tstamp;
		new_minc_lambda = std::max(
			new_minc_lambda, (this_segs_sent * MS_TO_SECS) / this_time_window);
	}

	beliefs.min_c_lambda = std::max(beliefs.min_c_lambda, new_minc_lambda);
    // TODO: Implement timeout
}

void SlowConv::update_beliefs(Time now, SegmentData seg,
							  bool updated_history,
							  TimeDelta time_since_last_update) {
	beliefs.min_rtt = std::min(beliefs.min_rtt, seg.rtt);
    TimeDelta jitter = beliefs.min_rtt * JITTER_MULTIPLIER;

    beliefs.min_qdel = std::max((TimeDelta) 0, seg.rtt - beliefs.min_rtt - jitter);
    beliefs.bq_belief1 = cum_segs_sent - cum_segs_delivered - cum_segs_lost;

    if(updated_history) {
        SeqNumDelta estimated_sent = cum_segs_sent - beliefs.last_segs_sent;
        SeqNumDelta estimated_delivered = beliefs.min_c_lambda * time_since_last_update;
        beliefs.bq_belief2 = beliefs.bq_belief2 + estimated_sent - estimated_delivered;
        beliefs.last_segs_sent = cum_segs_sent;

		update_beliefs_minc_maxc(now);
		update_beliefs_minc_lambda(now);
	}
}

void SlowConv::update_history(Time now, SegmentData seg) {
	TimeDelta inter_history_time = INTER_HISTORY_TIME * beliefs.min_rtt;
	TimeDelta time_since_last_update = now - last_history_update_time;
	if (time_since_last_update >= inter_history_time) {
		last_history_update_time = now;
		History h = History({now, seg.rtt, seg.rtt, cum_segs_sent,
							 cum_segs_delivered, cum_segs_lost, sending_rate,
							 seg.cum_segs_delivered_at_send, seg.this_loss_count});
		history.push_back(h);
		update_beliefs(now, seg, true, time_since_last_update);
	} else {
		History &latest = history.back();
		latest.interval_max_rtt = std::max(latest.interval_max_rtt, seg.rtt);
		latest.interval_min_rtt = std::min(latest.interval_min_rtt, seg.rtt);
		update_beliefs(now, seg, false, time_since_last_update);
	}
}

void SlowConv::set_rate_cwnd() {

}
