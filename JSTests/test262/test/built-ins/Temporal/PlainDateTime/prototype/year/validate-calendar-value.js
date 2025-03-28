// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-get-temporal.plaindatetime.prototype.year
description: Validate result returned from calendar year() method
features: [Temporal]
---*/

const badResults = [
  [undefined, RangeError],
  [Infinity, RangeError],
  [-Infinity, RangeError],
  [Symbol("foo"), TypeError],
  [7n, TypeError],
];

badResults.forEach(([result, error]) => {
  const calendar = new class extends Temporal.Calendar {
    year() {
      return result;
    }
  }("iso8601");
  const instance = new Temporal.PlainDateTime(1981, 12, 15, 14, 15, 45, 987, 654, 321, calendar);
  assert.throws(error, () => instance.year, `${typeof result} not converted to integer`);
});

const convertedResults = [
  [null, 0],
  [true, 1],
  [false, 0],
  [7.1, 7],
  [-7, -7],
  [-0.1, 0],
  [NaN, 0],
  ["string", 0],
  ["7", 7],
  ["7.5", 7],
  [{}, 0],
  [{valueOf() { return 7; }}, 7],
];

convertedResults.forEach(([result, convertedResult]) => {
  const calendar = new class extends Temporal.Calendar {
    year() {
      return result;
    }
  }("iso8601");
  const instance = new Temporal.PlainDateTime(1981, 12, 15, 14, 15, 45, 987, 654, 321, calendar);
  assert.sameValue(instance.year, convertedResult, `${typeof result} converted to integer ${convertedResult}`);
});
