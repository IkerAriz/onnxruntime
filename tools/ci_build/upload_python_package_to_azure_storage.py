#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

import os
import argparse
import warnings
import subprocess
import logging

log = logging.getLogger("Build")


def parse_nightly_and_local_version_from_whl_name(blob_name):
    night_build = 'nightly' if blob_name.find(".dev") > 0 else 'stable'

    start = blob_name.find("+")
    if start == -1:
        return night_build, None
    start = start + 1
    end = blob_name.find("-", start)
    if end == -1:
        return night_build, None
    return night_build, blob_name[start:end]


def run_subprocess(args, cwd=None):
    log.debug("Running subprocess in '{0}'\n{1}".format(cwd or os.getcwd(), args))
    return subprocess.run(args, cwd=cwd, check=True)


def upload_whl(python_wheel_path):
    blob_name = os.path.basename(python_wheel_path)
    run_subprocess(['azcopy', 'cp', python_wheel_path, 'https://onnxruntimepackages.blob.core.windows.net/$web/'])

    nightly_build, local_version = parse_nightly_and_local_version_from_whl_name(blob_name)
    if local_version:
        html_blob_name = 'onnxruntime_{}_{}.html'.format(nightly_build, local_version)
    else:
        html_blob_name = 'onnxruntime_{}.html'.format(nightly_build)

    download_path_to_html = "./onnxruntime_{}.html".format(nightly_build)

    run_subprocess(['azcopy', 'cp', 'https://onnxruntimepackages.blob.core.windows.net/$web/'+html_blob_name,
                    download_path_to_html])

    blob_name_plus_replaced = blob_name.replace('+', '%2B')
    with open(download_path_to_html) as f:
        lines = f.read().splitlines()

    new_line = '<a href="{blobname}">{blobname}</a><br>'.format(blobname=blob_name_plus_replaced)
    if new_line not in lines:
        lines.append(new_line)
        lines.sort()

        with open(download_path_to_html, 'w') as f:
            for item in lines:
                f.write("%s\n" % item)
    else:
        warnings.warn("'{}' exists in {}. The html file is not updated.".format(new_line, download_path_to_html))
    run_subprocess(['azcopy', 'cp', download_path_to_html,
                    'https://onnxruntimepackages.blob.core.windows.net/$web/'+html_blob_name,
                    '--content-type', 'text/html', '--overwrite', 'true'])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Upload python whl to azure storage.")

    parser.add_argument("--python_wheel_path", type=str, help="path to python wheel")

    args = parser.parse_args()

    upload_whl(args.python_wheel_path)
