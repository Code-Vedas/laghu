// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

import http from "k6/http";
import { check } from "k6";

const baseOptions = {
  vus: Number(__ENV.VUS || 10),
  iterations: Number(__ENV.ITERATIONS || 100),
  summaryTrendStats: ["avg", "med", "p(90)", "p(95)", "p(99)"],
};

export const options = __ENV.DURATION
  ? {
      scenarios: {
        sustained: {
          executor: "ramping-vus",
          stages: [
            { duration: __ENV.RAMP_UP_DURATION || "5s", target: Number(__ENV.VUS || 10) },
            { duration: __ENV.DURATION, target: Number(__ENV.VUS || 10) },
            { duration: "1s", target: 0 },
          ],
        },
      },
      summaryTrendStats: baseOptions.summaryTrendStats,
    }
  : __ENV.STEADY_DURATION
    ? {
        scenarios: {
          sustained: {
            executor: "constant-vus",
            vus: Number(__ENV.VUS || 10),
            duration: __ENV.STEADY_DURATION,
          },
        },
        summaryTrendStats: baseOptions.summaryTrendStats,
      }
  : baseOptions;

export default function () {
  if (__ENV.EXECUTE_JAVASCRIPT) {
    const program = http.get(`${__ENV.BASE_URL}${__ENV.EXECUTE_JAVASCRIPT}`);
    const executable = check(program, { "JavaScript HTTP 200": (value) => value.status === 200 });
    let evaluated = false;
    if (executable) {
      try {
        new Function(program.body)();
        evaluated = true;
      } catch (_) {}
    }
    check(null, { "JavaScript executes": () => evaluated });
    return;
  }
  const paths = __ENV.REQUEST_PATHS ? JSON.parse(__ENV.REQUEST_PATHS) : [__ENV.REQUEST_PATH];
  const headers = __ENV.REQUEST_HEADERS ? JSON.parse(__ENV.REQUEST_HEADERS) : { Accept: __ENV.ACCEPT || "*/*" };
  const response = http.get(`${__ENV.BASE_URL}${paths[__ITER % paths.length]}`, { headers });
  check(response, { "HTTP 200": (value) => value.status === 200 });
}
