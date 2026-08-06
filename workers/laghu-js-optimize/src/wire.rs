// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

use anyhow::{Context, Result};

pub(crate) fn c_string(bytes: &[u8]) -> Result<&str> {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .context("unterminated queue string")?;
    std::str::from_utf8(&bytes[..end]).context("queue string is not UTF-8")
}

pub(crate) fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(
        bytes[offset..offset + 4]
            .try_into()
            .expect("fixed queue layout"),
    )
}

pub(crate) fn read_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .expect("fixed queue layout"),
    )
}

pub(crate) fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

pub(crate) fn write_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn preserves_little_endian_fixed_width_values() {
        let mut bytes = [0_u8; 20];
        write_u32(&mut bytes, 1, 0x0403_0201);
        write_u64(&mut bytes, 8, 0x0807_0605_0403_0201);
        assert_eq!(&bytes[1..5], &[1, 2, 3, 4]);
        assert_eq!(&bytes[8..16], &[1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(read_u32(&bytes, 1), 0x0403_0201);
        assert_eq!(read_u64(&bytes, 8), 0x0807_0605_0403_0201);
    }

    #[test]
    fn decodes_only_terminated_utf8_strings() {
        assert_eq!(c_string(b"laghu\0ignored").unwrap(), "laghu");
        assert!(c_string(b"unterminated").is_err());
        assert!(c_string(&[0xff, 0]).is_err());
    }
}
