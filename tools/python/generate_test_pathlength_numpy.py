"""
Uses numpy to compute the high-precision path length of the PathLength testcases in PathSpline_tests.cc

Run with: bazel run //donner/svg/core:generate_test_pathlength_numpy
"""

import numpy as np
from scipy.integrate import quad


def bezier(t, P0, P1, P2, P3):
    """Compute a point on a cubic Bezier curve."""
    return (
        (1 - t) ** 3 * P0
        + 3 * (1 - t) ** 2 * t * P1
        + 3 * (1 - t) * t**2 * P2
        + t**3 * P3
    )


def bezier_derivative(t, P0, P1, P2, P3):
    """Compute the derivative of a cubic Bezier curve at point t."""
    return (
        3 * (1 - t) ** 2 * (P1 - P0)
        + 6 * (1 - t) * t * (P2 - P1)
        + 3 * t**2 * (P3 - P2)
    )


def curve_length(P0, P1, P2, P3):
    """Compute the length of a cubic Bezier curve using numerical integration."""
    integrand = lambda t: np.linalg.norm(bezier_derivative(t, P0, P1, P2, P3))
    length, _ = quad(integrand, 0, 1)
    return length


# Define control points for different curve types
P0 = np.array([0, 0])

# Simple Curve
P1_simple = np.array([1, 2])
P2_simple = np.array([3, 2])
P3_simple = np.array([4, 0])

# Loop
P1_loop = np.array([1, 2])
P2_loop = np.array([3, -2])
P3_loop = np.array([4, 0])

# Cusp
P1_cusp = np.array([1, 2])
P2_cusp = np.array([2, 2])
P3_cusp = np.array([3, 0])

# Inflection Point
P1_inflection = np.array([1, 2])
P2_inflection = np.array([2, -2])
P3_inflection = np.array([3, 0])

# Collinear Control Points
P1_collinear = np.array([1, 1])
P2_collinear = np.array([2, 2])
P3_collinear = np.array([3, 3])

# Compute lengths
length_simple = curve_length(P0, P1_simple, P2_simple, P3_simple)
length_loop = curve_length(P0, P1_loop, P2_loop, P3_loop)
length_cusp = curve_length(P0, P1_cusp, P2_cusp, P3_cusp)
length_inflection = curve_length(P0, P1_inflection, P2_inflection, P3_inflection)
length_collinear = curve_length(P0, P1_collinear, P2_collinear, P3_collinear)

print(f"PathLengthSimpleCurve: {length_simple:.8f}")
print(f"PathLengthLoop: {length_loop:.8f}")
print(f"PathLengthCusp: {length_cusp:.8f}")
print(f"PathLengthInflectionPoint: {length_inflection:.8f}")
print(f"PathLengthCollinearControlPoints: {length_collinear:.8f}")
