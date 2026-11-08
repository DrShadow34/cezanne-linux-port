# Broadcast orbits from the IGS (BKG mirror): the daily merged navigation
# file of all constellations, refreshed through the day. Yesterday's file
# is merged in so the first hours of a UTC day still have current records.
import datetime
import gzip
import os
import urllib.request

URL = 'https://igs.bkg.bund.de/root_ftp/IGS/BRDC/{y}/{d:03d}/BRDC00WRD_S_{y}{d:03d}0000_01D_MN.rnx.gz'
MAX_AGE = 3600		# s before the file is fetched again


def fetch(day, timeout=30):
    """the day's RINEX text or None"""
    url = URL.format(y=day.year, d=day.timetuple().tm_yday)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return gzip.decompress(r.read()).decode('ascii', 'replace')
    except (OSError, EOFError, ValueError):
        return None


def cached(cache):
    """the cached file without fetching, or None"""
    path = os.path.join(cache, 'brdc.rnx')
    return path if os.path.exists(path) else None


def refresh(cache, force=False):
    """cache/brdc.rnx = yesterday's + today's records (UTC days) with today's
    header; returns its path, or None if there is no file at all"""
    path = os.path.join(cache, 'brdc.rnx')
    try:
        fresh = datetime.datetime.now(datetime.timezone.utc).timestamp() - os.stat(path).st_mtime < MAX_AGE
    except FileNotFoundError:
        fresh = False
    if fresh and not force:
        return path
    today = datetime.datetime.now(datetime.timezone.utc).date()
    texts = [t for t in (fetch(today - datetime.timedelta(days=1)), fetch(today)) if t]
    if texts:
        head, _, _ = texts[-1].partition('END OF HEADER')
        head += texts[-1][len(head):].split('\n', 1)[0] + '\n'
        body = ''.join(t.split('END OF HEADER', 1)[1].split('\n', 1)[1] for t in texts)
        os.makedirs(cache, exist_ok=True)
        with open(path + '.new', 'w') as f:
            f.write(head + body)
        os.replace(path + '.new', path)
    return path if os.path.exists(path) else None
