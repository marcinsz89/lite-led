#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SYSMODULE_DIR="${SCRIPT_DIR}/sysmodule"
OVERLAY_DIR="${SCRIPT_DIR}/overlay"
TOOLBOX_JSON="${SYSMODULE_DIR}/toolbox.json"

if [[ ! -f "${TOOLBOX_JSON}" ]]; then
    echo "Missing toolbox.json: ${TOOLBOX_JSON}" >&2
    exit 1
fi

if [[ -z "${DEVKITPRO:-}" ]]; then
    echo "DEVKITPRO is not set. Export DEVKITPRO before running this script." >&2
    exit 1
fi

package_name="$(sed -n 's/.*"name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${TOOLBOX_JSON}" | head -n 1)"
title_id="$(sed -n 's/.*"tid"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${TOOLBOX_JSON}" | head -n 1)"

if [[ -z "${package_name}" || -z "${title_id}" ]]; then
    echo "Failed to parse package name or title ID from ${TOOLBOX_JSON}" >&2
    exit 1
fi

products_dir="${SCRIPT_DIR}/products"
sysmodule_output="${products_dir}/sysmodule/${package_name}.nsp"
overlay_output="${products_dir}/overlay/${package_name}.ovl"

if [[ $# -ge 1 ]]; then
    parent_output_dir="$1"
else
    default_parent_output_dir="${SCRIPT_DIR}"
    read -r -p "Output parent path [${default_parent_output_dir}]: " parent_output_dir
    parent_output_dir="${parent_output_dir:-${default_parent_output_dir}}"
fi

parent_output_dir="${parent_output_dir%/}"
if [[ -z "${parent_output_dir}" ]]; then
    parent_output_dir="/"
fi

if [[ "$(basename "${parent_output_dir}")" == "${package_name}" ]]; then
    output_dir="${parent_output_dir}"
else
    output_dir="${parent_output_dir}/${package_name}"
fi

if [[ -e "${output_dir}" && ! -d "${output_dir}" ]]; then
    echo "Output path exists but is not a directory: ${output_dir}" >&2
    exit 1
fi

if [[ -d "${output_dir}" ]]; then
    read -r -p "Clear existing package directory ${output_dir}? [y/N]: " clear_output_dir
    case "${clear_output_dir}" in
        [Yy]|[Yy][Ee][Ss])
            find "${output_dir}" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
            ;;
        *)
            echo "Aborted."
            exit 1
            ;;
    esac
fi

echo "Building sysmodule..."
make -C "${SYSMODULE_DIR}" clean
make -C "${SYSMODULE_DIR}"

echo "Building overlay..."
make -C "${OVERLAY_DIR}" clean
make -C "${OVERLAY_DIR}"

if [[ ! -f "${sysmodule_output}" ]]; then
    echo "Missing sysmodule build output: ${sysmodule_output}" >&2
    exit 1
fi

if [[ ! -f "${overlay_output}" ]]; then
    echo "Missing overlay build output: ${overlay_output}" >&2
    exit 1
fi

echo "Packaging into ${output_dir}..."
mkdir -p "${output_dir}/atmosphere/contents/${title_id}/flags"
mkdir -p "${output_dir}/switch/.overlays"

cp "${sysmodule_output}" "${output_dir}/atmosphere/contents/${title_id}/exefs.nsp"
cp "${TOOLBOX_JSON}" "${output_dir}/atmosphere/contents/${title_id}/toolbox.json"
cp "${overlay_output}" "${output_dir}/switch/.overlays/${package_name}.ovl"
: > "${output_dir}/atmosphere/contents/${title_id}/flags/boot2.flag"

echo "Package ready at ${output_dir}"
