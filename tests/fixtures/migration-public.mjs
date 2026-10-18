// Published demonstration keys, never user credentials. Independent of our encoder.
// Version 2, three accounts:
// https://github.com/dfarnham/totp-qr/blob/master/README.md
export const version2='otpauth-migration://offline?data=Ci0KCkhlbGxvId6tvu8SEnRlc3QxQGV4YW1wbGUxLmNvbRoFVGVzdDEgASgBMAIKLQoKSGVsbG8h3q2%2B8BISdGVzdDJAZXhhbXBsZTIuY29tGgVUZXN0MiABKAEwAgotCgpIZWxsbyHerb7xEhJ0ZXN0M0BleGFtcGxlMy5jb20aBVRlc3QzIAEoATACEAIYASAA';

// No version, batch metadata, algorithm or digits (protobuf defaults):
// https://github.com/digitalduke/otpauth-migration-decoder/issues/6
export const withoutMetadata='otpauth-migration://offline?data=CjEKCkhlbGxvId6tvu8SGEV4YW1wbGU6YWxpY2VAZ29vZ2xlLmNvbRoHRXhhbXBsZTAC';
