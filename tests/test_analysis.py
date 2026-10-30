"""Deterministic clock, session, loss and deadline cases; no network/device."""
import contextlib
import csv
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analysis_common import ClockSamples, FrameIndex, arguments, MOD
import analyze_probe
import analyze_stream
from check_vmtest import check_stage_times


def write(path, rows):
    with path.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def sender(frame=0, session=7, at=1_000_000, count=4):
    return dict(session=session, frame=frame, bytes=count * 1356, packets=count, capture_us=at,
                encoded_us=at + 1000, first_send_us=at + 1100, last_send_us=at + 1400, dropped_packets=0)


def receiver(frame=0, session=7, at=1_000_000, **changes):
    row = dict(session=session, frame=frame, span=1, fate="shown", packets=4, packets_expected=4,
               first_rx_ns=(at + 6200) * 1000, complete_ns=(at + 6500) * 1000,
               decoded_ns=(at + 7500) * 1000, flip_ns=(at + 9000) * 1000,
               flip_sequence=frame + 50, capture_us=at)
    row.update(changes)
    return row


def clock(at=1_000_000, rtt=200, offset=5000, session=7):
    return dict(session=session, sender_tx_us=at, receiver_rx_us=at + offset + rtt // 2,
                receiver_tx_us=at + offset + rtt // 2 + 10, sender_rx_us=at + rtt + 10)


class AnalysisTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.base = Path(self.directory.name)

    def args(self, senders, receivers, clocks=None, packets=None):
        write(self.base / "tx.csv", senders)
        write(self.base / "rx.csv", receivers)
        if clocks:
            write(self.base / "tx.csv.clock.csv", clocks)
        argv = ["--sender", str(self.base / "tx.csv"), "--receiver", str(self.base / "rx.csv"), "--drift-ppm", "0"]
        if packets is not None:
            write(self.base / "packets.csv", packets)
            argv += ["--packets", str(self.base / "packets.csv")]
        return arguments("test", argv, probe=packets is not None)

    def test_mapping_and_percentiles_and_missing_stages(self):
        args = self.args([sender(), sender(1, at=1_020_000)],
                         [receiver(), receiver(1, at=1_020_000, decoded_ns=0, flip_ns=0, fate="delivered")],
                         [clock(rtt=600, offset=5200), clock(at=999_000), clock(at=20_000_000, rtt=2, offset=99)])
        result = analyze_stream.analyze(args)
        self.assertEqual(result["clock"]["offset_us"]["median"], 5000)
        self.assertEqual(result["clock"]["uncertainty_us"]["median"], 101)
        self.assertAlmostEqual(result["intervals"]["sent_to_first_rx"]["median"], .1)
        self.assertAlmostEqual(result["intervals"]["capture_to_flip"]["median"], 4)
        self.assertEqual(result["intervals"]["capture_to_flip"]["n"], 1)
        self.assertEqual(result["fates"]["delivered"], 1)
        args.clock_window = .00001
        result = analyze_stream.analyze(args)
        self.assertEqual(result["clock"]["unmapped_frames"], 2)
        self.assertEqual(result["intervals"]["capture_to_flip"]["n"], 0)
        self.assertEqual(result["intervals"]["capture_to_encoded"]["n"], 2)

    def test_file_only_profiles_and_independent_stage_check(self):
        args = self.args([sender(session=0), sender()], [receiver()], [clock()])
        result = analyze_stream.analyze(args)
        self.assertEqual(result['matched_frames'], 1)
        self.assertTrue(any('file-only' in warning for warning in result['warnings']))
        row = sender()
        stage = dict(acquire_return_us=1000100, conversion_submit_us=1000200,
                     encode_start_us=1000300, send_return_us=1001500)
        received = receiver(encoded_us=1000)
        check_stage_times(row, stage, received)
        with self.assertRaisesRegex(ValueError, 'out of order'):
            check_stage_times(row, dict(stage, encode_start_us=1001900), received)
        # Changing both duplicate lock columns must still fail the independent
        # wire timestamp read from the receiver's profile.
        with self.assertRaisesRegex(ValueError, 'wire timestamp'):
            check_stage_times(dict(row, encoded_us=1000900), dict(stage, bitstream_locked_us=1000900), received)

    def test_sessions_spans_wrap_repeated_vblanks(self):
        rows = [receiver(frame=MOD - 1, fate="lost", span=3, capture_us=0, first_rx_ns=0,
                         complete_ns=0, decoded_ns=0, flip_ns=0)]
        index = FrameIndex(rows)
        for frame in (MOD - 1, 0, 1):
            self.assertIs(index.get(7, frame), rows[0])
        self.assertIsNone(index.get(8, 0))
        self.assertIsNone(index.get(7, 2))
        with self.assertRaises(ValueError):
            FrameIndex(rows + [receiver()])
        args = self.args([sender(), sender(session=8, at=2_000_000)],
                         [receiver(flip_sequence=MOD - 1), receiver(session=8, at=2_000_000,
                            flip_sequence=2, flip_ns=2_013_000_000)],
                         [clock(), clock(at=2_000_000, session=8, offset=9000)])
        result = analyze_stream.analyze(args)
        self.assertEqual(result["matched_frames"], 2)
        self.assertEqual(result["intervals"]["capture_to_flip"]["median"], 4)
        self.assertEqual(result["repeated_vblanks_between_logged_flips"], 2)
        huge = FrameIndex([receiver(frame=10, span=1 << 30, fate="lost")])
        self.assertIsNotNone(huge.get(7, (1 << 30) + 9))

    def test_session_required_and_mismatch_rejected(self):
        old = sender()
        del old["session"]
        args = self.args([old], [receiver()], [clock()])
        with self.assertRaisesRegex(ValueError, "session"):
            analyze_stream.analyze(args)
        write(self.base / "tx.csv", [sender()])
        self.assertEqual(analyze_stream.analyze(args)["matched_frames"], 1)
        selected = arguments("test", ["--sender", str(args.sender), "--receiver", str(args.receiver), "--session", "7"])
        self.assertEqual(analyze_stream.analyze(selected)["matched_frames"], 1)
        for option, value in (("--sender-log", "sender.log"),):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                arguments("test", ["--sender", str(args.sender), "--receiver", str(args.receiver), option, value])
        old_clock = clock()
        del old_clock["session"]
        write(self.base / "tx.csv.clock.csv", [old_clock])
        with self.assertRaisesRegex(ValueError, "session"):
            analyze_stream.analyze(args)
        write(self.base / "tx.csv.clock.csv", [clock()])
        write(self.base / "rx.csv", [receiver(capture_us=123)])
        with self.assertRaisesRegex(ValueError, "timestamps disagree"):
            analyze_stream.analyze(args)

    def test_clock_invalid_sample_and_sign(self):
        rows = [clock(offset=-5000), clock()]
        rows[1]["receiver_tx_us"] += 5000
        clocks = ClockSamples(rows)
        self.assertEqual(clocks.invalid, 1)
        self.assertEqual(clocks.choose(7, 1_000_000, 10000)["offset_us"], -5000)

    def packets(self, positions, *, late=False):
        rows = []
        previous_rx = 0
        for pos in positions:
            frame, index = divmod(pos, 4)
            row = dict(session=7, sequence=pos, frame=frame, index=index, count=4,
                       rx_ns=(1_006_200 + frame * 20_000 + index * 100) * 1000,
                       kernel_drops=0, timestamp_fallback=0)
            if late and pos == 7:
                row["rx_ns"] += 30_000_000
            row["rx_ns"] = max(row["rx_ns"], previous_rx + 100_000)
            previous_rx = row["rx_ns"]
            rows.append(row)
        return rows

    def probe(self, positions, *, rtt=200, late=False):
        return self.args([sender(), sender(1, at=1_020_000)], [receiver(), receiver(1, at=1_020_000)],
                         [clock(at=990_000 + i * 10_000, rtt=rtt) for i in range(4)],
                         self.packets(positions, late=late))

    def test_exact_bursts_duplicates_reorder_and_no_retransmission(self):
        # Gaps 1..2, 4. Arrival reordering and duplicate 3 are not losses.
        args = self.probe([0, 3, 6, 5, 3, 7])
        result = analyze_probe.analyze(args)
        self.assertEqual(result["missing_packets"], 3)
        self.assertEqual(result["burst_length_histogram_packets"], {1: 1, 2: 1})
        self.assertEqual(result["duplicate_packets"], 1)
        self.assertEqual(result["out_of_order_packets"], 1)
        self.assertEqual(result["recommendation"], "FEC")
        self.assertNotIn("nack", result)
        self.assertEqual(result["missing_data_packets"], 3)
        self.assertEqual(result["missing_parity_packets"], 0)

    def test_late_frames_and_observed_loss(self):
        args = self.probe(list(range(8)), late=True)
        result = analyze_probe.analyze(args)
        self.assertEqual(result["missing_packets"], 0)
        self.assertEqual(result["frames_late"], 1)
        self.assertEqual(result["recommendation"], "no FEC")
        args = self.probe([0, 2, 3, 4, 5, 6, 7], rtt=30_000)
        result = analyze_probe.analyze(args)
        self.assertEqual(result["recommendation"], "FEC")

    def test_whole_and_final_losses_need_more_evidence(self):
        result = analyze_probe.analyze(self.probe([0, 1, 2]))
        self.assertEqual(result["frames_wholly_missing"], 1)
        self.assertEqual(result["burst_length_histogram_packets"], {5: 1})
        self.assertEqual(result["recommendation"], "no decision")

    def test_socket_loss_and_missing_packet_trace(self):
        args = self.probe([0, 2, 3, 4, 5, 6, 7])
        log = self.base / "run.log"
        log.write_text("Receiver packets=9 kernel_drops=1 offset_pair_uncertainty_ns=200\n")
        args.receiver_log = log
        result = analyze_probe.analyze(args)
        self.assertEqual(result["kernel_socket_drops"], 1)
        self.assertEqual(result["recommendation"], "no decision")

        args.packets = None
        result = analyze_probe.analyze(args)
        self.assertIsNone(result["missing_packets"])
        self.assertIsNone(result["burst_length_histogram_packets"])
        self.assertEqual(result["recommendation"], "no decision")

    def test_collector_loss_is_not_network_loss(self):
        args = self.probe(list(range(8)))
        self.assertEqual(analyze_probe.analyze(args)["recommendation"], "no FEC")
        (self.base / "profile.jsonl").write_text(json.dumps({"message": "#skipped packets 12"}) + "\n")
        result = analyze_probe.analyze(args)
        self.assertEqual(result["recommendation"], "no decision")
        self.assertEqual(result["receiver_counters"]["profile_records_missing"], 12)
        self.assertTrue(any("Profile collection" in warning for warning in result["warnings"]))

    def test_injected_loss_does_not_make_a_wifi_recommendation(self):
        args = self.probe([0, 2, 3, 4, 5, 6, 7])
        tx = [sender(), sender(1, at=1_020_000)]
        tx[0]["dropped_packets"] = 1
        write(self.base / "tx.csv", tx)
        result = analyze_probe.analyze(args)
        self.assertEqual(result["missing_packets"], 1)
        self.assertEqual(result["unexplained_missing_packets"], 0)
        self.assertEqual(result["recommendation"], "no decision")
        self.assertIn("deliberate", result["recommendation_reason"])

    def test_no_observations_do_not_support_a_loss_claim(self):
        self.assertIn("no sender packets", analyze_probe.decision({"expected_packets": 0})[1])

    def test_fec_readiness_interleaving_and_late_optional_parity(self):
        # 129 data -> 2 blocks, 2 parity. Missing data 0 and 1 are in different
        # blocks. One parity from each block is enough; two from just one aren't.
        arrivals = {i: i + 1000 for i in range(2, 129)}
        arrivals.update({129: 2000, 130: 3000})
        self.assertEqual(analyze_probe.ready_time(arrivals, 129, 2), 3000)
        del arrivals[130]
        self.assertIsNone(analyze_probe.ready_time(arrivals, 129, 2))
        arrivals.update({0: 10, 1: 20, 130: 1000000000})
        self.assertEqual(analyze_probe.ready_time(arrivals, 129, 2), 1128)

    def fec_probe(self, *, omit=(), late_data=False):
        tx = [sender(i, at=1_000_000 + i * 20_000) for i in range(2)]
        for row in tx:
            row.update(packets=5, fec_packets=1)
        rx = [receiver(i, at=1_000_000 + i * 20_000, fate="delivered", damaged=0,
                       frames_repaired=int(i == 0), fragments_repaired=int(i == 0), fec_packets=1)
              for i in range(2)]
        packets = []
        for f in range(2):
            for i in range(5):
                if (f, i) in omit:
                    continue
                at = 1_006_200 + f * 20_000 + i * 100
                if late_data and f == 0 and i == 1:
                    at += 30000
                if f == 1 and i == 4:
                    at += 30000  # Parity arriving after a clean AU is optional.
                packets.append(dict(session=7, frame=f, sequence=f * 5 + i, index=i,
                                    count=4, fec_count=1, fec_scheme=1, rx_ns=at * 1000))
        return self.args(tx, rx, packets=packets)

    def test_fec_trace_counts_and_damage_join(self):
        args = self.fec_probe(omit=((0, 1), (1, 4)))
        result = analyze_probe.analyze(args)
        self.assertEqual(result["missing_data_packets"], 1)
        self.assertEqual(result["missing_parity_packets"], 1)
        self.assertEqual(result["frames_incomplete"], 0)
        self.assertEqual(result["frames_late"], 0)
        self.assertEqual(result["frames_timely_only_with_fec"], 1)
        self.assertEqual(result["frames_repaired"], 1)
        self.assertEqual(result["fragments_repaired"], 1)
        self.assertEqual(result["recommendation"], "FEC")
        args.joined = self.base / "joined.csv"
        stream = analyze_stream.analyze(args)
        self.assertEqual(stream["frames_repaired"], 1)
        self.assertEqual(stream["damage_status_unknown_frames"], 0)
        self.assertIn("damaged,frames_repaired,fragments_repaired", args.joined.read_text())
        # Late data eventually arrives; FEC still prevented a missed deadline.
        result = analyze_probe.analyze(self.fec_probe(late_data=True))
        self.assertEqual(result["missing_packets"], 0)
        self.assertEqual(result["frames_timely_only_with_fec"], 1)
        self.assertEqual(result["frames_late"], 0)
        self.assertEqual(result["recommendation"], "FEC")

    def test_definitions_are_verbose_but_always_in_json(self):
        args = self.probe(list(range(8)))
        for module in (analyze_probe, analyze_stream):
            argv = ["--sender", str(args.sender), "--receiver", str(args.receiver),
                    "--json", str(self.base / "summary.json")]
            if module is analyze_probe:
                argv += ["--packets", str(args.packets)]
            plain, verbose = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(plain):
                self.assertEqual(module.main(argv), 0)
            summary = json.loads((self.base / "summary.json").read_text())
            self.assertTrue(summary["definitions"])
            for definition in summary["definitions"]:
                self.assertNotIn(definition, plain.getvalue())
            with contextlib.redirect_stdout(verbose):
                self.assertEqual(module.main(argv + ["--verbose"]), 0)
            self.assertEqual(json.loads((self.base / "summary.json").read_text()), summary)
            for definition in summary["definitions"]:
                self.assertIn(definition, verbose.getvalue())
            if module is analyze_probe:
                self.assertEqual(plain.getvalue().splitlines()[-1], summary["recommendation_reason"])

    def test_damaged_fate_is_independent_and_legacy_unknown(self):
        args = self.args([sender(), sender(1, at=1_020_000)],
                         [receiver(damaged=1), receiver(1, at=1_020_000, damaged=0)])
        result = analyze_stream.analyze(args)
        self.assertEqual(result["damaged_frames"], 1)
        self.assertEqual(result["fates"]["shown"], 2)
        self.assertEqual(result["repair_status_unknown_frames"], 2)
        result = analyze_stream.analyze(self.args([sender()], [receiver()]))
        self.assertEqual(result["damage_status_unknown_frames"], 1)


if __name__ == "__main__":
    unittest.main()
