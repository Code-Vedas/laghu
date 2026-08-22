// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

use std::{
    fs::{self, File, OpenOptions},
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, bail};
use fs2::FileExt;
use memmap2::{MmapMut, MmapOptions};

use crate::persisted_state::*;
use crate::wire::{read_u32, read_u64, write_u32, write_u64};

pub(crate) const FAILURE_CACHE: usize = 1;
pub(crate) const FAILURE_QUEUE: usize = 2;
pub(crate) const FAILURE_TRANSFORM: usize = 4;
const STALE_SECONDS: u64 = 45;
const LATENCY_LIMITS: [u64; 11] = [
    1_000, 5_000, 10_000, 25_000, 50_000, 100_000, 250_000, 500_000, 1_000_000, 2_500_000,
    5_000_000,
];
static DIAGNOSTIC_EMITTED: AtomicBool = AtomicBool::new(false);

pub(crate) enum Operational {
    Active(OperationalRegistry),
    Unavailable,
}

pub(crate) struct OperationalRegistry {
    file: File,
    pub(crate) map: MmapMut,
    pub(crate) slot: usize,
    generation: u64,
}

impl OperationalRegistry {
    pub(crate) fn open(cache: &Path) -> Result<Self> {
        fs::create_dir_all(cache)?;
        let path = operational_path(cache);
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .open(path)?;
        file.try_lock_exclusive()?;
        let size = usize::try_from(OPERATIONAL_HEADER_SIZE)?
            .checked_add(
                usize::try_from(OPERATIONAL_SLOT_COUNT)?
                    .checked_mul(usize::try_from(OPERATIONAL_SLOT_SIZE)?)
                    .context("operational registry size overflow")?,
            )
            .context("operational registry size overflow")?;
        if file.metadata()?.len() == 0 {
            file.set_len(size as u64)?;
        }
        if file.metadata()?.len() != size as u64 {
            FileExt::unlock(&file)?;
            bail!("incompatible operational registry size");
        }
        let mut map = unsafe { MmapOptions::new().map_mut(&file)? };
        if read_u64(&map, OPERATIONAL_HEADER_MAGIC_OFFSET as usize) == 0 {
            map.fill(0);
            write_u64(
                &mut map,
                OPERATIONAL_HEADER_MAGIC_OFFSET as usize,
                OPERATIONAL_MAGIC,
            );
            write_u32(
                &mut map,
                OPERATIONAL_HEADER_VERSION_OFFSET as usize,
                OPERATIONAL_VERSION,
            );
            write_u32(
                &mut map,
                OPERATIONAL_HEADER_SLOT_COUNT_OFFSET as usize,
                OPERATIONAL_SLOT_COUNT,
            );
            write_u64(
                &mut map,
                OPERATIONAL_HEADER_GENERATION_OFFSET as usize,
                epoch_seconds(),
            );
        }
        if read_u64(&map, OPERATIONAL_HEADER_MAGIC_OFFSET as usize) != OPERATIONAL_MAGIC
            || read_u32(&map, OPERATIONAL_HEADER_VERSION_OFFSET as usize) != OPERATIONAL_VERSION
            || read_u32(&map, OPERATIONAL_HEADER_SLOT_COUNT_OFFSET as usize)
                != OPERATIONAL_SLOT_COUNT
        {
            FileExt::unlock(&file)?;
            bail!("incompatible operational registry");
        }
        let now = epoch_seconds();
        let slot = (0..OPERATIONAL_SLOT_COUNT as usize)
            .find(|slot| {
                let base = operational_slot_base(*slot);
                let sequence = read_u64(&map, base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize);
                let active = read_u32(&map, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize);
                let heartbeat = read_u64(&map, base + OPERATIONAL_SLOT_HEARTBEAT_OFFSET as usize);
                active == 0
                    || (sequence & 1 == 0
                        && (heartbeat == 0
                            || heartbeat > now
                            || now.saturating_sub(heartbeat) > STALE_SECONDS))
            })
            .context("operational registry has no free process slot")?;
        let generation =
            read_u64(&map, OPERATIONAL_HEADER_GENERATION_OFFSET as usize).saturating_add(1);
        write_u64(
            &mut map,
            OPERATIONAL_HEADER_GENERATION_OFFSET as usize,
            generation,
        );
        let base = operational_slot_base(slot);
        map[base..base + OPERATIONAL_SLOT_SIZE as usize].fill(0);
        write_u64(
            &mut map,
            base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
            1,
        );
        write_u64(
            &mut map,
            base + OPERATIONAL_SLOT_GENERATION_OFFSET as usize,
            generation,
        );
        write_u64(
            &mut map,
            base + OPERATIONAL_SLOT_HEARTBEAT_OFFSET as usize,
            now,
        );
        write_u32(&mut map, base + OPERATIONAL_SLOT_SURFACE_OFFSET as usize, 3);
        write_u32(
            &mut map,
            base + OPERATIONAL_SLOT_PROCESS_KIND_OFFSET as usize,
            2,
        );
        map[base + OPERATIONAL_SLOT_REQUIRED_OFFSET as usize] = 1;
        map[base + OPERATIONAL_SLOT_HEALTHY_OFFSET as usize] = 1;
        write_u32(&mut map, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize, 1);
        write_u64(
            &mut map,
            base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
            2,
        );
        map.flush_async()?;
        FileExt::unlock(&file)?;
        Ok(Self {
            file,
            map,
            slot,
            generation,
        })
    }

    pub(crate) fn update(&mut self, update: impl FnOnce(&mut [u8])) {
        if self.file.try_lock_exclusive().is_err() {
            return;
        }
        let base = operational_slot_base(self.slot);
        let valid = read_u64(
            &self.map,
            base + OPERATIONAL_SLOT_GENERATION_OFFSET as usize,
        ) == self.generation
            && read_u32(&self.map, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize) != 0;
        let sequence = read_u64(&self.map, base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize);
        if valid && sequence & 1 == 0 {
            write_u64(
                &mut self.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
                sequence.saturating_add(1),
            );
            update(&mut self.map[base..base + OPERATIONAL_SLOT_SIZE as usize]);
            write_u64(
                &mut self.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
                sequence.saturating_add(2),
            );
            let _ = self.map.flush_async();
        }
        let _ = FileExt::unlock(&self.file);
    }

    pub(crate) fn heartbeat(&mut self, capacity: u64, occupied: u64) {
        self.update(|slot| {
            slot[OPERATIONAL_SLOT_HEALTHY_OFFSET as usize] = 1;
            write_u64(
                slot,
                OPERATIONAL_SLOT_QUEUE_CAPACITY_OFFSET as usize,
                capacity,
            );
            write_u64(
                slot,
                OPERATIONAL_SLOT_QUEUE_OCCUPIED_OFFSET as usize,
                occupied.min(capacity),
            );
            write_u64(
                slot,
                OPERATIONAL_SLOT_HEARTBEAT_OFFSET as usize,
                epoch_seconds(),
            );
        });
    }
    pub(crate) fn record(&mut self, elapsed_microseconds: u64, failure: Option<usize>) {
        self.update(|slot| {
            let bucket = LATENCY_LIMITS
                .iter()
                .position(|limit| elapsed_microseconds <= *limit)
                .unwrap_or(LATENCY_LIMITS.len());
            saturating_add(
                slot,
                OPERATIONAL_SLOT_LATENCY_BUCKETS_OFFSET as usize + bucket * 8,
                1,
            );
            saturating_add(slot, OPERATIONAL_SLOT_LATENCY_COUNT_OFFSET as usize, 1);
            saturating_add(
                slot,
                OPERATIONAL_SLOT_LATENCY_SUM_OFFSET as usize,
                elapsed_microseconds,
            );
            if let Some(failure) = failure.filter(|failure| *failure < 6) {
                saturating_add(
                    slot,
                    OPERATIONAL_SLOT_FAILURES_OFFSET as usize + failure * 8,
                    1,
                );
            }
        });
    }
    fn release(&mut self) {
        if self.file.try_lock_exclusive().is_err() {
            return;
        }
        let base = operational_slot_base(self.slot);
        let sequence = read_u64(&self.map, base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize);
        if sequence & 1 == 0
            && read_u64(
                &self.map,
                base + OPERATIONAL_SLOT_GENERATION_OFFSET as usize,
            ) == self.generation
            && read_u32(&self.map, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize) != 0
        {
            write_u64(
                &mut self.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
                sequence.saturating_add(1),
            );
            self.map[base + OPERATIONAL_SLOT_HEALTHY_OFFSET as usize] = 0;
            write_u32(
                &mut self.map,
                base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize,
                0,
            );
            write_u64(
                &mut self.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize,
                sequence.saturating_add(2),
            );
            let _ = self.map.flush();
        }
        let _ = FileExt::unlock(&self.file);
    }
}
impl Drop for OperationalRegistry {
    fn drop(&mut self) {
        self.release();
    }
}
impl Operational {
    pub(crate) fn open(cache: &Path) -> Self {
        match OperationalRegistry::open(cache) {
            Ok(registry) => Self::Active(registry),
            Err(_) => {
                if !DIAGNOSTIC_EMITTED.swap(true, Ordering::Relaxed) {
                    eprintln!("laghu-js-optimize: operational registry unavailable");
                }
                Self::Unavailable
            }
        }
    }
    pub(crate) fn heartbeat(&mut self, capacity: u64, occupied: u64) {
        if let Self::Active(registry) = self {
            registry.heartbeat(capacity, occupied);
        }
    }
    pub(crate) fn record(&mut self, elapsed_microseconds: u64, failure: Option<usize>) {
        if let Self::Active(registry) = self {
            registry.record(elapsed_microseconds, failure);
        }
    }
}
pub(crate) fn operational_slot_base(slot: usize) -> usize {
    OPERATIONAL_HEADER_SIZE as usize + slot * OPERATIONAL_SLOT_SIZE as usize
}
pub(crate) fn operational_path(cache: &Path) -> PathBuf {
    let mut path = cache.as_os_str().to_os_string();
    path.push(".laghu-operations");
    PathBuf::from(path)
}
fn saturating_add(bytes: &mut [u8], offset: usize, increment: u64) {
    write_u64(
        bytes,
        offset,
        read_u64(bytes, offset).saturating_add(increment),
    );
}
fn epoch_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |time| time.as_secs())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn publishes_operational_v4_heartbeat_metrics_and_release() {
        let root = std::env::temp_dir().join(format!(
            "laghu-js-operational-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let cache = root.join("cache");
        fs::create_dir_all(&root).unwrap();
        let mut registry = OperationalRegistry::open(&cache).unwrap();
        registry.heartbeat(8, 0);
        registry.record(1_200, Some(FAILURE_TRANSFORM));
        let base = operational_slot_base(registry.slot);
        assert_eq!(
            read_u64(&registry.map, OPERATIONAL_HEADER_MAGIC_OFFSET as usize),
            OPERATIONAL_MAGIC
        );
        assert_eq!(
            read_u32(&registry.map, OPERATIONAL_HEADER_VERSION_OFFSET as usize),
            OPERATIONAL_VERSION
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_SEQUENCE_OFFSET as usize
            ) & 1,
            0
        );
        assert_eq!(
            read_u32(
                &registry.map,
                base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize
            ),
            1
        );
        assert_eq!(
            read_u32(
                &registry.map,
                base + OPERATIONAL_SLOT_PROCESS_KIND_OFFSET as usize
            ),
            2
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_QUEUE_CAPACITY_OFFSET as usize
            ),
            8
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_QUEUE_OCCUPIED_OFFSET as usize
            ),
            0
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_LATENCY_COUNT_OFFSET as usize
            ),
            1
        );
        assert_eq!(
            read_u64(
                &registry.map,
                base + OPERATIONAL_SLOT_FAILURES_OFFSET as usize + FAILURE_TRANSFORM * 8
            ),
            1
        );
        drop(registry);
        let bytes = fs::read(operational_path(&cache)).unwrap();
        assert_eq!(
            read_u32(&bytes, base + OPERATIONAL_SLOT_ACTIVE_OFFSET as usize),
            0
        );
        assert_eq!(bytes[base + OPERATIONAL_SLOT_HEALTHY_OFFSET as usize], 0);
        fs::remove_file(operational_path(&cache)).unwrap();
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn incompatible_operational_registry_is_explicitly_unavailable() {
        let root = std::env::temp_dir().join(format!(
            "laghu-js-operational-unavailable-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&root).unwrap();
        fs::write(operational_path(&root), b"incompatible").unwrap();
        assert!(matches!(Operational::open(&root), Operational::Unavailable));
        fs::remove_file(operational_path(&root)).unwrap();
        fs::remove_dir_all(root).unwrap();
    }
}
