"""
Selene - éphémérides hors ligne : direction du Soleil et de la Terre vues depuis un point de la Lune.

Modèle analytique (aucun fichier, aucune connexion) :
  - Lune : série de Meeus (Astronomical Algorithms, ch. 47), 32 termes en longitude, 30 en latitude
  - Soleil : formules basse précision de Meeus (ch. 25)
  - orientation de la Lune : libration optique (Meeus, ch. 53), libration physique négligée
Précision attendue : quelques centièmes de degré. À citer dans le pitch ; JPL Horizons reste la référence.

Repère lunaire : x vers (lon 0, lat 0), y vers l'est (lon +90), z vers le pôle Nord.
Azimut : depuis le Nord local, sens horaire. Élévation : au-dessus de l'horizon local (sphère de 1737,4 km).
"""
import datetime as dt
import math

MOON_R_KM = 1737.4
AU_KM = 149597870.7
INCL = 1.54242          # inclinaison de l'équateur lunaire moyen sur l'écliptique (deg)

# (D, M, M', F, somme_l [1e-6 deg], somme_r [1e-3 km])
_LR = [
    (0, 0, 1, 0, 6288774, -20905355), (2, 0, -1, 0, 1274027, -3699111), (2, 0, 0, 0, 658314, -2955968),
    (0, 0, 2, 0, 213618, -569925), (0, 1, 0, 0, -185116, 48888), (0, 0, 0, 2, -114332, -3149),
    (2, 0, -2, 0, 58793, 246158), (2, -1, -1, 0, 57066, -152138), (2, 0, 1, 0, 53322, -170733),
    (2, -1, 0, 0, 45758, -204586), (0, 1, -1, 0, -40923, -129620), (1, 0, 0, 0, -34720, 108743),
    (0, 1, 1, 0, -30383, 104755), (2, 0, 0, -2, 15327, 10321), (0, 0, 1, 2, -12528, 0),
    (0, 0, 1, -2, 10980, 79661), (4, 0, -1, 0, 10675, -34782), (0, 0, 3, 0, 10034, -23210),
    (4, 0, -2, 0, 8548, -21636), (2, 1, -1, 0, -7888, 24208), (2, 1, 0, 0, -6766, 30824),
    (1, 0, -1, 0, -5163, -8379), (1, 1, 0, 0, 4987, -16675), (2, -1, 1, 0, 4036, -12831),
    (2, 0, 2, 0, 3994, -10445), (4, 0, 0, 0, 3861, -11650), (2, 0, -3, 0, 3665, 14403),
    (0, 1, -2, 0, -2689, -7003), (2, 0, -1, 2, -2602, 0), (2, -1, -2, 0, 2390, 10056),
    (1, 0, 1, 0, -2348, 6322), (2, -2, 0, 0, 2236, -9884),
]
# (D, M, M', F, somme_b [1e-6 deg])
_B = [
    (0, 0, 0, 1, 5128122), (0, 0, 1, 1, 280602), (0, 0, 1, -1, 277693), (2, 0, 0, -1, 173237),
    (2, 0, -1, 1, 55413), (2, 0, -1, -1, 46271), (2, 0, 0, 1, 32573), (0, 0, 2, 1, 17198),
    (2, 0, 1, -1, 9266), (0, 0, 2, -1, 8822), (2, -1, 0, -1, 8216), (2, 0, -2, -1, 4324),
    (2, 0, 1, 1, 4200), (2, 1, 0, -1, -3359), (2, -1, -1, 1, 2463), (2, -1, 0, 1, 2211),
    (2, -1, -1, -1, 2065), (0, 1, -1, -1, -1870), (4, 0, -1, -1, 1828), (0, 1, 0, 1, -1794),
    (0, 0, 0, 3, -1749), (0, 1, -1, 1, -1565), (1, 0, 0, 1, -1491), (0, 1, 1, 1, -1475),
    (0, 1, 1, -1, -1410), (0, 1, 0, -1, -1344), (1, 0, 0, -1, -1335), (0, 0, 3, 1, 1107),
    (4, 0, 0, -1, 1021), (4, 0, -1, 1, 833),
]


def _rad(x):
    return math.radians(x)


def julian_day(t):
    """JD d'une date UTC (datetime naïf en UTC)."""
    y, m = t.year, t.month
    d = t.day + (t.hour + (t.minute + t.second / 60.0) / 60.0) / 24.0
    if m <= 2:
        y -= 1
        m += 12
    a = y // 100
    b = 2 - a + a // 4
    return math.floor(365.25 * (y + 4716)) + math.floor(30.6001 * (m + 1)) + d + b - 1524.5


def moon_ecliptic(T):
    """Longitude, latitude (deg, écliptique de la date) et distance (km) de la Lune vue de la Terre."""
    Lp = 218.3164477 + 481267.88123421 * T - 0.0015786 * T * T
    D = 297.8501921 + 445267.1114034 * T - 0.0018819 * T * T
    M = 357.5291092 + 35999.0502909 * T - 0.0001536 * T * T
    Mp = 134.9633964 + 477198.8675055 * T + 0.0087414 * T * T
    F = 93.2720950 + 483202.0175233 * T - 0.0036539 * T * T
    E = 1 - 0.002516 * T - 0.0000074 * T * T
    A1 = 119.75 + 131.849 * T
    A2 = 53.09 + 479264.290 * T
    A3 = 313.45 + 481266.484 * T
    sl = sr = sb = 0.0
    for d, m, mp, f, cl, cr in _LR:
        e = E ** abs(m)
        arg = _rad(d * D + m * M + mp * Mp + f * F)
        sl += cl * e * math.sin(arg)
        sr += cr * e * math.cos(arg)
    for d, m, mp, f, cb in _B:
        e = E ** abs(m)
        sb += cb * e * math.sin(_rad(d * D + m * M + mp * Mp + f * F))
    sl += 3958 * math.sin(_rad(A1)) + 1962 * math.sin(_rad(Lp - F)) + 318 * math.sin(_rad(A2))
    sb += (-2235 * math.sin(_rad(Lp)) + 382 * math.sin(_rad(A3)) + 175 * math.sin(_rad(A1 - F))
           + 175 * math.sin(_rad(A1 + F)) + 127 * math.sin(_rad(Lp - Mp)) - 115 * math.sin(_rad(Lp + Mp)))
    lon = (Lp + sl / 1e6) % 360.0
    lat = sb / 1e6
    dist = 385000.56 + sr / 1e3
    return lon, lat, dist, F


def sun_ecliptic(T):
    """Longitude (deg) et distance (km) du Soleil vu de la Terre."""
    L0 = 280.46646 + 36000.76983 * T + 0.0003032 * T * T
    M = 357.52911 + 35999.05029 * T - 0.0001537 * T * T
    e = 0.016708634 - 0.000042037 * T
    C = ((1.914602 - 0.004817 * T) * math.sin(_rad(M)) + (0.019993 - 0.000101 * T) * math.sin(_rad(2 * M))
         + 0.000289 * math.sin(_rad(3 * M)))
    nu = M + C
    R = 1.000001018 * (1 - e * e) / (1 + e * math.cos(_rad(nu)))
    return (L0 + C) % 360.0, R * AU_KM


def _libration(lon, lat, F, T):
    """Longitude et latitude sélénographiques (deg, est positif) du point où l'observateur lointain
    situé dans la direction (lon, lat) écliptique de la Lune verrait le zénith (Meeus ch. 53)."""
    omega = 125.0445479 - 1934.1362891 * T + 0.0020754 * T * T
    W = _rad(lon - omega)
    b = _rad(lat)
    I = _rad(INCL)
    A = math.atan2(math.sin(W) * math.cos(b) * math.cos(I) - math.sin(b) * math.sin(I), math.cos(W) * math.cos(b))
    l = math.degrees(A) - F
    l = (l + 180.0) % 360.0 - 180.0
    bb = math.degrees(math.asin(-math.sin(W) * math.cos(b) * math.sin(I) - math.sin(b) * math.cos(I)))
    return l, bb


def _unit(l_deg, b_deg):
    l, b = _rad(l_deg), _rad(b_deg)
    return (math.cos(b) * math.cos(l), math.cos(b) * math.sin(l), math.sin(b))


def _azel(v, lat, lon):
    la, lo = _rad(lat), _rad(lon)
    n = (math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la))
    east = (-math.sin(lo), math.cos(lo), 0.0)
    north = (-math.sin(la) * math.cos(lo), -math.sin(la) * math.sin(lo), math.cos(la))
    dot = lambda a, b: a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    norm = math.sqrt(dot(v, v))
    az = math.degrees(math.atan2(dot(v, east), dot(v, north))) % 360.0
    el = math.degrees(math.asin(dot(v, n) / norm))
    return az, el


def body_directions(t_utc):
    """(l, b, distance_km) du point sub-terrestre et (l0, b0) du point sub-solaire, pour une date UTC."""
    jd = julian_day(t_utc + dt.timedelta(seconds=69.2))          # UTC -> TT
    T = (jd - 2451545.0) / 36525.0
    mlon, mlat, mdist, F = moon_ecliptic(T)
    slon, sdist = sun_ecliptic(T)
    # Lune vue du Soleil : vecteur Lune - Soleil dans l'écliptique
    cm = (mdist * math.cos(_rad(mlat)) * math.cos(_rad(mlon)),
          mdist * math.cos(_rad(mlat)) * math.sin(_rad(mlon)),
          mdist * math.sin(_rad(mlat)))
    cs = (sdist * math.cos(_rad(slon)), sdist * math.sin(_rad(slon)), 0.0)
    h = (cm[0] - cs[0], cm[1] - cs[1], cm[2] - cs[2])
    hlon = math.degrees(math.atan2(h[1], h[0])) % 360.0
    hlat = math.degrees(math.atan2(h[2], math.hypot(h[0], h[1])))
    l, b = _libration(mlon, mlat, F, T)
    l0, b0 = _libration(hlon, hlat, F, T)
    return (l, b, mdist), (l0, b0, sdist)


def azel_series(lat, lon, start, n, step_min):
    """Liste de (sun_az, sun_el, earth_az, earth_el) pour n pas depuis 'start' (datetime UTC naïf)."""
    site = _unit(lon, lat)
    out = []
    for i in range(n):
        (l, b, md), (l0, b0, sd) = body_directions(start + dt.timedelta(minutes=step_min * i))
        e = _unit(l, b)
        s = _unit(l0, b0)
        ev = tuple(md * e[k] - MOON_R_KM * site[k] for k in range(3))       # parallaxe de la Terre incluse
        sv = tuple(sd * s[k] - MOON_R_KM * site[k] for k in range(3))
        sa, se = _azel(sv, lat, lon)
        ea, ee = _azel(ev, lat, lon)
        out.append((sa, se, ea, ee))
    return out


def vectors(start, n, step_min):
    """Pour chaque pas : (direction unitaire du Soleil, vecteur Terre en km), dans le repère lunaire
    (x = lon 0/lat 0, y = est, z = nord). Permet de calculer, pour CHAQUE cellule, la hauteur et la direction des astres."""
    out = []
    for i in range(n):
        (l, b, md), (l0, b0, sd) = body_directions(start + dt.timedelta(minutes=step_min * i))
        out.append((_unit(l0, b0), tuple(md * c for c in _unit(l, b))))
    return out
