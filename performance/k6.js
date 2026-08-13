// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

import http from "k6/http";
import { check } from "k6";

export const options = {
  vus: Number(__ENV.VUS || 10),
  iterations: Number(__ENV.ITERATIONS || 100),
  summaryTrendStats: ["avg", "med", "p(95)", "p(99)"],
};

export default function () {
  const response = http.get(`${__ENV.BASE_URL}${__ENV.REQUEST_PATH}`, { headers: { Accept: __ENV.ACCEPT || "*/*" } });
  check(response, { "HTTP 200": (value) => value.status === 200 });
}
