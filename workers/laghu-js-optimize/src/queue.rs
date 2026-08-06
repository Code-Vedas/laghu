// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

use crate::persisted_state::*;
use crate::wire::{c_string, read_u32, read_u64, write_u32, write_u64};
use crate::{BACKEND, MAX_INPUT};
use anyhow::{Context, Result, bail};
use fs2::FileExt;
use memmap2::{MmapMut, MmapOptions};
use std::{
    fs::{self, File, OpenOptions},
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};

pub(crate) const JOB_JAVASCRIPT: u32 = 3;
const FILTER_MODULE: u64 = 1;
pub(crate) const FILTER_SOURCE_MAP: u64 = 2;

pub(crate) struct Queue {
    pub(crate) file: File,
    pub(crate) map: MmapMut,
    pub(crate) slots: usize,
    pub(crate) payload_size: usize,
    pub(crate) last_advertised: Option<u64>,
}

pub(crate) struct Job {
    pub(crate) index_key: String,
    pub(crate) request_path: String,
    pub(crate) validator: String,
    pub(crate) policy_key: String,
    pub(crate) target: String,
    pub(crate) module: bool,
    pub(crate) source_map: bool,
    pub(crate) source: Vec<u8>,
}

impl Queue {
    pub(crate) fn create(path: &Path) -> Result<Self> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let slots = 8usize;
        let length =
            QUEUE_HEADER_SIZE as usize + slots * (QUEUE_SLOT_HEADER_SIZE as usize + MAX_INPUT);
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)?;
        file.set_len(length as u64)?;
        let mut map = unsafe { MmapOptions::new().map_mut(&file)? };
        map.fill(0);
        write_u64(&mut map, QUEUE_HEADER_MAGIC_OFFSET as usize, QUEUE_MAGIC);
        write_u32(
            &mut map,
            QUEUE_HEADER_VERSION_OFFSET as usize,
            QUEUE_VERSION,
        );
        write_u32(
            &mut map,
            QUEUE_HEADER_SLOT_COUNT_OFFSET as usize,
            slots as u32,
        );
        write_u64(
            &mut map,
            QUEUE_HEADER_SLOT_PAYLOAD_SIZE_OFFSET as usize,
            MAX_INPUT as u64,
        );
        map.flush()?;
        Ok(Self {
            file,
            map,
            slots,
            payload_size: MAX_INPUT,
            last_advertised: None,
        })
    }

    pub(crate) fn open(path: &Path) -> Result<Self> {
        let file = OpenOptions::new().read(true).write(true).open(path)?;
        let map = unsafe { MmapOptions::new().map_mut(&file)? };
        if map.len() < QUEUE_HEADER_SIZE as usize
            || read_u64(&map, QUEUE_HEADER_MAGIC_OFFSET as usize) != QUEUE_MAGIC
            || read_u32(&map, QUEUE_HEADER_VERSION_OFFSET as usize) != QUEUE_VERSION
        {
            bail!("incompatible javascript queue");
        }
        let slots = read_u32(&map, QUEUE_HEADER_SLOT_COUNT_OFFSET as usize) as usize;
        let payload_size = usize::try_from(read_u64(
            &map,
            QUEUE_HEADER_SLOT_PAYLOAD_SIZE_OFFSET as usize,
        ))?;
        let expected = (QUEUE_HEADER_SIZE as usize)
            .checked_add(
                slots
                    .checked_mul(
                        (QUEUE_SLOT_HEADER_SIZE as usize)
                            .checked_add(payload_size)
                            .context("queue size overflow")?,
                    )
                    .context("queue size overflow")?,
            )
            .context("queue size overflow")?;
        if slots == 0 || payload_size < MAX_INPUT || map.len() != expected {
            bail!("invalid javascript queue dimensions");
        }
        Ok(Self {
            file,
            map,
            slots,
            payload_size,
            last_advertised: None,
        })
    }

    pub(crate) fn advertise(&mut self) -> Result<()> {
        let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
        if self.last_advertised == Some(now) {
            return Ok(());
        }
        if self.file.try_lock_exclusive().is_err() {
            return Ok(());
        }
        let result: Result<()> = (|| {
            write_u32(&mut self.map, QUEUE_HEADER_CAPABILITIES_OFFSET as usize, 1);
            write_u64(&mut self.map, QUEUE_HEADER_HEARTBEAT_OFFSET as usize, now);
            self.map[QUEUE_HEADER_BACKEND_ID_OFFSET as usize..QUEUE_HEADER_SIZE as usize].fill(0);
            self.map[QUEUE_HEADER_BACKEND_ID_OFFSET as usize
                ..QUEUE_HEADER_BACKEND_ID_OFFSET as usize + BACKEND.len()]
                .copy_from_slice(BACKEND.as_bytes());
            Ok(())
        })();
        let unlock = FileExt::unlock(&self.file);
        if let Err(error) = result {
            let _ = unlock;
            return Err(error);
        }
        unlock?;
        self.last_advertised = Some(now);
        Ok(())
    }

    pub(crate) fn take(&mut self) -> Result<Option<Job>> {
        if self.file.try_lock_exclusive().is_err() {
            return Ok(None);
        }
        let result = (|| {
            let read =
                read_u32(&self.map, QUEUE_HEADER_NEXT_READ_OFFSET as usize) as usize % self.slots;
            let base = queue_slot_base(read, self.payload_size);
            if read_u32(&self.map, base + QUEUE_SLOT_STATE_OFFSET as usize) != QUEUE_SLOT_READY {
                return Ok(None);
            }
            let length = usize::try_from(read_u64(
                &self.map,
                base + QUEUE_SLOT_PAYLOAD_LENGTH_OFFSET as usize,
            ))?;
            let valid = read_u32(&self.map, base + QUEUE_SLOT_KIND_OFFSET as usize)
                == JOB_JAVASCRIPT
                && length > 0
                && length <= MAX_INPUT
                && length <= self.payload_size;
            let job = if valid {
                Some(Job {
                    index_key: c_string(
                        &self.map[base + QUEUE_SLOT_INDEX_KEY_OFFSET as usize
                            ..base + QUEUE_SLOT_REQUEST_PATH_OFFSET as usize],
                    )?
                    .to_owned(),
                    request_path: c_string(
                        &self.map[base + QUEUE_SLOT_REQUEST_PATH_OFFSET as usize
                            ..base + QUEUE_SLOT_VALIDATOR_OFFSET as usize],
                    )?
                    .to_owned(),
                    validator: c_string(
                        &self.map[base + QUEUE_SLOT_VALIDATOR_OFFSET as usize
                            ..base + QUEUE_SLOT_CONTENT_TYPE_OFFSET as usize],
                    )?
                    .to_owned(),
                    policy_key: c_string(
                        &self.map[base + QUEUE_SLOT_POLICY_KEY_OFFSET as usize
                            ..base + QUEUE_SLOT_PROVIDER_ID_OFFSET as usize],
                    )?
                    .to_owned(),
                    target: c_string(
                        &self.map[base + QUEUE_SLOT_JAVASCRIPT_TARGET_OFFSET as usize
                            ..base + QUEUE_SLOT_FILTERS_OFFSET as usize],
                    )?
                    .to_owned(),
                    module: read_u64(&self.map, base + QUEUE_SLOT_FILTERS_OFFSET as usize)
                        & FILTER_MODULE
                        != 0,
                    source_map: read_u64(&self.map, base + QUEUE_SLOT_FILTERS_OFFSET as usize)
                        & FILTER_SOURCE_MAP
                        != 0,
                    source: self.map[base + QUEUE_SLOT_HEADER_SIZE as usize
                        ..base + QUEUE_SLOT_HEADER_SIZE as usize + length]
                        .to_vec(),
                })
            } else {
                None
            };
            self.map[base..base + QUEUE_SLOT_HEADER_SIZE as usize + length.min(self.payload_size)]
                .fill(0);
            write_u32(
                &mut self.map,
                QUEUE_HEADER_NEXT_READ_OFFSET as usize,
                ((read + 1) % self.slots) as u32,
            );
            Ok(job)
        })();
        let unlock = FileExt::unlock(&self.file);
        if let Err(error) = result {
            let _ = unlock;
            return Err(error);
        }
        unlock?;
        result
    }

    pub(crate) fn snapshot(&self) -> (u64, u64) {
        let occupied = (0..self.slots)
            .filter(|slot| {
                read_u32(
                    &self.map,
                    queue_slot_base(*slot, self.payload_size) + QUEUE_SLOT_STATE_OFFSET as usize,
                ) == QUEUE_SLOT_READY
            })
            .count();
        (self.slots as u64, occupied as u64)
    }
}

pub(crate) fn queue_slot_base(slot: usize, payload_size: usize) -> usize {
    QUEUE_HEADER_SIZE as usize + slot * (QUEUE_SLOT_HEADER_SIZE as usize + payload_size)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn advertises_queue_once_per_second() {
        let root = std::env::temp_dir().join(format!(
            "laghu-js-advertise-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let queue_path = root.join("javascript.queue");
        fs::create_dir_all(&root).unwrap();
        let mut queue = Queue::create(&queue_path).unwrap();
        queue.advertise().unwrap();
        let advertised = queue.last_advertised;
        queue.advertise().unwrap();
        assert_eq!(queue.last_advertised, advertised);
        assert_eq!(
            read_u32(&queue.map, QUEUE_HEADER_CAPABILITIES_OFFSET as usize),
            1
        );
        assert_eq!(
            &queue.map[QUEUE_HEADER_BACKEND_ID_OFFSET as usize
                ..QUEUE_HEADER_BACKEND_ID_OFFSET as usize + BACKEND.len()],
            BACKEND.as_bytes()
        );
        drop(queue);
        fs::remove_dir_all(root).unwrap();
    }
}
