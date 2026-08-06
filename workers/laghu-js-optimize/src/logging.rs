// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::json;

fn timestamp() -> String {
    let seconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |value| value.as_secs());
    let days = seconds / 86_400;
    let seconds_of_day = seconds % 86_400;
    let z = days as i64 + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let year = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let month_parameter = (5 * doy + 2) / 153;
    let day = doy - (153 * month_parameter + 2) / 5 + 1;
    let month = month_parameter + if month_parameter < 10 { 3 } else { -9 };
    let year = year + if month <= 2 { 1 } else { 0 };
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        seconds_of_day / 3_600,
        (seconds_of_day / 60) % 60,
        seconds_of_day % 60
    )
}

pub(crate) fn lifecycle(state: &str, failure: &str) {
    eprintln!(
        "{}",
        json!({
            "schema": "laghu-log-v1",
            "timestamp": timestamp(),
            "surface": "worker",
            "component": "js-optimize",
            "event": "lifecycle",
            "state": state,
            "failure": failure
        })
    );
}

pub(crate) fn job(success: bool, input_bytes: usize, output_bytes: usize, duration_ms: u64) {
    eprintln!(
        "{}",
        json!({
            "schema": "laghu-log-v1",
            "timestamp": timestamp(),
            "surface": "worker",
            "component": "js-optimize",
            "event": "job",
            "job_kind": "javascript",
            "outcome": if success { "success" } else { "failed" },
            "input_bytes": input_bytes,
            "output_bytes": output_bytes,
            "duration_ms": duration_ms,
            "failure": if success { "none" } else { "transform" }
        })
    );
}
