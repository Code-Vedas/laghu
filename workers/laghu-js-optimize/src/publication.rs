// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

use crate::persisted_state::*;
use crate::queue::Job;
use crate::{BACKEND, MAX_INPUT, sha256, write_u32, write_u64};
use anyhow::{Result, bail};

const CATALOG_MAGIC: u64 = 0x4c41_4748_554a_5343;
const CATALOG_SIZE: usize = 1_908;
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};

pub(crate) fn write_fixed(target: &mut [u8], value: &str) -> Result<()> {
    if value.len() >= target.len() {
        bail!("cache metadata field is too large");
    }
    target.fill(0);
    target[..value.len()].copy_from_slice(value.as_bytes());
    Ok(())
}

pub(crate) fn atomic_write(path: &Path, data: &[u8]) -> Result<()> {
    let temporary = path.with_extension(format!("tmp-{}", std::process::id()));
    {
        let mut file = OpenOptions::new()
            .create(true)
            .truncate(true)
            .write(true)
            .open(&temporary)?;
        file.write_all(data)?;
        file.sync_all()?;
    }
    fs::rename(temporary, path)?;
    Ok(())
}

pub(crate) fn publish(
    cache: &Path,
    job: &Job,
    output: &[u8],
    source_map: &[u8],
) -> Result<Vec<u8>> {
    if job.index_key.len() != 64 || job.validator.len() != 64 {
        bail!("invalid javascript cache key");
    }
    fs::create_dir_all(cache)?;
    let mut published = output.to_vec();
    if !source_map.is_empty() {
        let map_variant = sha256(source_map);
        let directive = format!("\n//# sourceMappingURL=/.laghu/js/{map_variant}.map");
        if published.len() + directive.len() >= job.source.len()
            || published.len() + directive.len() > MAX_INPUT
        {
            bail!("javascript with source map reference is not smaller");
        }
        atomic_write(
            &cache.join(format!("variant-{map_variant}.bin")),
            source_map,
        )?;
        let mut map_metadata = vec![0u8; CACHE_ARTIFACT_SIZE as usize];
        write_u64(
            &mut map_metadata,
            CACHE_ARTIFACT_MAGIC_OFFSET as usize,
            CACHE_ARTIFACT_MAGIC,
        );
        write_u32(
            &mut map_metadata,
            CACHE_ARTIFACT_VERSION_OFFSET as usize,
            CACHE_ARTIFACT_VERSION,
        );
        write_u64(
            &mut map_metadata,
            CACHE_ARTIFACT_LENGTH_OFFSET as usize,
            source_map.len() as u64,
        );
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_VARIANT_KEY_OFFSET as usize
                ..CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET as usize],
            &map_variant,
        )?;
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET as usize
                ..CACHE_ARTIFACT_VALIDATOR_OFFSET as usize],
            &map_variant,
        )?;
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_VALIDATOR_OFFSET as usize
                ..CACHE_ARTIFACT_CONTENT_TYPE_OFFSET as usize],
            &map_variant,
        )?;
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_CONTENT_TYPE_OFFSET as usize
                ..CACHE_ARTIFACT_BACKEND_ID_OFFSET as usize],
            "application/json",
        )?;
        write_fixed(
            &mut map_metadata[CACHE_ARTIFACT_BACKEND_ID_OFFSET as usize
                ..CACHE_ARTIFACT_BACKEND_ID_END_OFFSET as usize],
            BACKEND,
        )?;
        atomic_write(
            &cache.join(format!("index-{map_variant}.meta")),
            &map_metadata,
        )?;
        published.extend_from_slice(directive.as_bytes());
    }
    let variant = sha256(&published);
    atomic_write(&cache.join(format!("variant-{variant}.bin")), &published)?;
    let mut metadata = vec![0u8; CACHE_ARTIFACT_SIZE as usize];
    write_u64(
        &mut metadata,
        CACHE_ARTIFACT_MAGIC_OFFSET as usize,
        CACHE_ARTIFACT_MAGIC,
    );
    write_u32(
        &mut metadata,
        CACHE_ARTIFACT_VERSION_OFFSET as usize,
        CACHE_ARTIFACT_VERSION,
    );
    write_u64(
        &mut metadata,
        CACHE_ARTIFACT_LENGTH_OFFSET as usize,
        published.len() as u64,
    );
    write_fixed(
        &mut metadata[CACHE_ARTIFACT_VARIANT_KEY_OFFSET as usize
            ..CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET as usize],
        &variant,
    )?;
    write_fixed(
        &mut metadata
            [CACHE_ARTIFACT_PAYLOAD_HASH_OFFSET as usize..CACHE_ARTIFACT_VALIDATOR_OFFSET as usize],
        &variant,
    )?;
    write_fixed(
        &mut metadata
            [CACHE_ARTIFACT_VALIDATOR_OFFSET as usize..CACHE_ARTIFACT_CONTENT_TYPE_OFFSET as usize],
        &job.validator,
    )?;
    write_fixed(
        &mut metadata[CACHE_ARTIFACT_CONTENT_TYPE_OFFSET as usize
            ..CACHE_ARTIFACT_BACKEND_ID_OFFSET as usize],
        "application/javascript",
    )?;
    write_fixed(
        &mut metadata[CACHE_ARTIFACT_BACKEND_ID_OFFSET as usize
            ..CACHE_ARTIFACT_BACKEND_ID_END_OFFSET as usize],
        BACKEND,
    )?;
    atomic_write(
        &cache.join(format!("index-{}.meta", job.index_key)),
        &metadata,
    )?;
    atomic_write(&cache.join(format!("index-{variant}.meta")), &metadata)?;
    Ok(published)
}

pub(crate) fn catalog_key(job: &Job) -> String {
    sha256(
        format!(
            "laghu-js-url-v1\n{}\n{}\n{}\n{}",
            job.request_path,
            job.policy_key,
            job.target,
            if job.module { "module" } else { "classic" }
        )
        .as_bytes(),
    )
}

pub(crate) fn publish_catalog(
    cache: &Path,
    job: &Job,
    output: &[u8],
    matrix: &str,
    flags: u32,
) -> Result<()> {
    if !job.request_path.starts_with('/') || job.policy_key.len() != 64 {
        return Ok(());
    }
    let variant = sha256(output);
    let matrix_digest = sha256(matrix.as_bytes());
    atomic_write(
        &cache.join(format!("javascript-matrix-{matrix_digest}.txt")),
        matrix.as_bytes(),
    )?;
    let mut catalog = vec![0u8; CATALOG_SIZE];
    write_u64(&mut catalog, 0, CATALOG_MAGIC);
    write_u32(&mut catalog, 8, 3);
    write_u32(&mut catalog, 12, u32::from(job.module));
    write_u64(
        &mut catalog,
        16,
        SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
    );
    write_u64(&mut catalog, 24, job.source.len() as u64);
    write_u64(&mut catalog, 32, output.len() as u64);
    write_fixed(&mut catalog[40..1064], &job.request_path)?;
    write_fixed(&mut catalog[1064..1129], &job.policy_key)?;
    write_fixed(&mut catalog[1129..1641], &job.target)?;
    write_fixed(&mut catalog[1641..1706], &job.validator)?;
    write_fixed(&mut catalog[1706..1771], &variant)?;
    write_fixed(&mut catalog[1771..1836], &matrix_digest)?;
    write_u32(&mut catalog, 1836, flags);
    let checksum = sha256(&catalog[..1840]);
    write_fixed(&mut catalog[1840..1905], &checksum)?;
    atomic_write(
        &cache.join(format!("javascript-{}.meta", catalog_key(job))),
        &catalog,
    )
}
