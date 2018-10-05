// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
* Copyright (c) 2004-2016, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: April 20, 2004
* Since: ICU 3.0
**********************************************************************
*/
#include "utypeinfo.h"  // for 'typeid' to work
#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/measfmt.h"
#include "unicode/numfmt.h"
#include "currfmt.h"
#include "unicode/localpointer.h"
#include "resource.h"
#include "unicode/simpleformatter.h"
#include "quantityformatter.h"
#include "unicode/plurrule.h"
#include "unicode/decimfmt.h"
#include "uresimp.h"
#include "unicode/ures.h"
#include "unicode/ustring.h"
#include "ureslocs.h"
#include "cstring.h"
#include "mutex.h"
#include "ucln_in.h"
#include "unicode/listformatter.h"
#include "charstr.h"
#include "unicode/putil.h"
#include "unicode/smpdtfmt.h"
#include "uassert.h"
#include "unicode/uameasureformat.h"
#include "fphdlimp.h"

#include "sharednumberformat.h"
#include "sharedpluralrules.h"
#include "standardplural.h"
#include "unifiedcache.h"


U_NAMESPACE_BEGIN

static constexpr int32_t PER_UNIT_INDEX = StandardPlural::COUNT;
static constexpr int32_t PATTERN_COUNT = PER_UNIT_INDEX + 1;
static constexpr int32_t MEAS_UNIT_COUNT = 138;  // see assertion in MeasureFormatCacheData constructor
static constexpr int32_t WIDTH_INDEX_COUNT = UMEASFMT_WIDTH_NARROW + 1;

UOBJECT_DEFINE_RTTI_IMPLEMENTATION(MeasureFormat)

// Used to format durations like 5:47 or 21:35:42.
class NumericDateFormatters : public UMemory {
public:
    // Formats like H:mm
    SimpleDateFormat hourMinute;

    // formats like M:ss
    SimpleDateFormat minuteSecond;

    // formats like H:mm:ss
    SimpleDateFormat hourMinuteSecond;

    // Constructor that takes the actual patterns for hour-minute,
    // minute-second, and hour-minute-second respectively.
    NumericDateFormatters(
            const UnicodeString &hm,
            const UnicodeString &ms,
            const UnicodeString &hms,
            UErrorCode &status) : 
            hourMinute(hm, status),
            minuteSecond(ms, status), 
            hourMinuteSecond(hms, status) {
        const TimeZone *gmt = TimeZone::getGMT();
        hourMinute.setTimeZone(*gmt);
        minuteSecond.setTimeZone(*gmt);
        hourMinuteSecond.setTimeZone(*gmt);
    }
private:
    NumericDateFormatters(const NumericDateFormatters &other);
    NumericDateFormatters &operator=(const NumericDateFormatters &other);
};

static UMeasureFormatWidth getRegularWidth(UMeasureFormatWidth width) {
    if (width >= WIDTH_INDEX_COUNT) {
        return UMEASFMT_WIDTH_NARROW;
    }
    return width;
}

/**
 * Instances contain all MeasureFormat specific data for a particular locale.
 * This data is cached. It is never copied, but is shared via shared pointers.
 *
 * Note: We might change the cache data to have an array[WIDTH_INDEX_COUNT] of
 * complete sets of unit & per patterns,
 * to correspond to the resource data and its aliases.
 *
 * TODO: Maybe store more sparsely in general, with pointers rather than potentially-empty objects.
 */
class MeasureFormatCacheData : public SharedObject {
public:

    /**
     * Redirection data from root-bundle, top-level sideways aliases.
     * - UMEASFMT_WIDTH_COUNT: initial value, just fall back to root
     * - UMEASFMT_WIDTH_WIDE/SHORT/NARROW: sideways alias for missing data
     */
    UMeasureFormatWidth widthFallback[WIDTH_INDEX_COUNT];
    /** Measure unit -> format width -> array of patterns ("{0} meters") (plurals + PER_UNIT_INDEX) */
    SimpleFormatter* patterns[MEAS_UNIT_COUNT][WIDTH_INDEX_COUNT][PATTERN_COUNT];
    const UChar* dnams[MEAS_UNIT_COUNT][WIDTH_INDEX_COUNT];
    SimpleFormatter perFormatters[WIDTH_INDEX_COUNT];

    MeasureFormatCacheData();
    virtual ~MeasureFormatCacheData();

    UBool hasPerFormatter(int32_t width) const {
        // TODO: Create a more obvious way to test if the per-formatter has been set?
        // Use pointers, check for NULL? Or add an isValid() method?
        return perFormatters[width].getArgumentLimit() == 2;
    }

    void adoptCurrencyFormat(int32_t widthIndex, NumberFormat *nfToAdopt) {
        delete currencyFormats[widthIndex];
        currencyFormats[widthIndex] = nfToAdopt;
    }
    const NumberFormat *getCurrencyFormat(UMeasureFormatWidth width) const {
        return currencyFormats[getRegularWidth(width)];
    }
    void adoptIntegerFormat(NumberFormat *nfToAdopt) {
        delete integerFormat;
        integerFormat = nfToAdopt;
    }
    const NumberFormat *getIntegerFormat() const {
        return integerFormat;
    }
    void adoptNumericDateFormatters(NumericDateFormatters *formattersToAdopt) {
        delete numericDateFormatters;
        numericDateFormatters = formattersToAdopt;
    }
    const NumericDateFormatters *getNumericDateFormatters() const {
        return numericDateFormatters;
    }

private:
    NumberFormat* currencyFormats[WIDTH_INDEX_COUNT];
    NumberFormat* integerFormat;
    NumericDateFormatters* numericDateFormatters;

    MeasureFormatCacheData(const MeasureFormatCacheData &other);
    MeasureFormatCacheData &operator=(const MeasureFormatCacheData &other);
};

MeasureFormatCacheData::MeasureFormatCacheData()
        : integerFormat(nullptr), numericDateFormatters(nullptr) {
    // Please update MEAS_UNIT_COUNT if it gets out of sync with the true count!
    U_ASSERT(MEAS_UNIT_COUNT == MeasureUnit::getIndexCount());

    for (int32_t i = 0; i < WIDTH_INDEX_COUNT; ++i) {
        widthFallback[i] = UMEASFMT_WIDTH_COUNT;
    }
    memset(&patterns[0][0][0], 0, sizeof(patterns));
    memset(&dnams[0][0], 0, sizeof(dnams));
    memset(currencyFormats, 0, sizeof(currencyFormats));
}

MeasureFormatCacheData::~MeasureFormatCacheData() {
    for (int32_t i = 0; i < UPRV_LENGTHOF(currencyFormats); ++i) {
        delete currencyFormats[i];
    }
    for (int32_t i = 0; i < MEAS_UNIT_COUNT; ++i) {
        for (int32_t j = 0; j < WIDTH_INDEX_COUNT; ++j) {
            for (int32_t k = 0; k < PATTERN_COUNT; ++k) {
                delete patterns[i][j][k];
            }
        }
    }
    // Note: the contents of 'dnams' are pointers into the resource bundle
    delete integerFormat;
    delete numericDateFormatters;
}

static UBool isCurrency(const MeasureUnit &unit) {
    return (uprv_strcmp(unit.getType(), "currency") == 0);
}

static UBool getString(
        const UResourceBundle *resource,
        UnicodeString &result,
        UErrorCode &status) {
    int32_t len = 0;
    const UChar *resStr = ures_getString(resource, &len, &status);
    if (U_FAILURE(status)) {
        return FALSE;
    }
    result.setTo(TRUE, resStr, len);
    return TRUE;
}

namespace {

static const UChar g_LOCALE_units[] = {
    0x2F, 0x4C, 0x4F, 0x43, 0x41, 0x4C, 0x45, 0x2F,
    0x75, 0x6E, 0x69, 0x74, 0x73
};
static const UChar gShort[] = { 0x53, 0x68, 0x6F, 0x72, 0x74 };
static const UChar gNarrow[] = { 0x4E, 0x61, 0x72, 0x72, 0x6F, 0x77 };

/**
 * Sink for enumerating all of the measurement unit display names.
 * Contains inner sink classes, each one corresponding to a type of resource table.
 * The outer sink handles the top-level units, unitsNarrow, and unitsShort tables.
 *
 * More specific bundles (en_GB) are enumerated before their parents (en_001, en, root):
 * Only store a value if it is still missing, that is, it has not been overridden.
 *
 * C++: Each inner sink class has a reference to the main outer sink.
 * Java: Use non-static inner classes instead.
 */
struct UnitDataSink : public ResourceSink {

    // Output data.
    MeasureFormatCacheData &cacheData;

    // Path to current data.
    UMeasureFormatWidth width;
    const char *type;
    int32_t unitIndex;

    UnitDataSink(MeasureFormatCacheData &outputData)
            : cacheData(outputData),
              width(UMEASFMT_WIDTH_COUNT), type(NULL), unitIndex(0) {}
    ~UnitDataSink();

    void setFormatterIfAbsent(int32_t index, const ResourceValue &value,
                                int32_t minPlaceholders, UErrorCode &errorCode) {
        U_ASSERT(unitIndex < MEAS_UNIT_COUNT);
        U_ASSERT(width < WIDTH_INDEX_COUNT);
        U_ASSERT(index < PATTERN_COUNT);
        SimpleFormatter **patterns = &cacheData.patterns[unitIndex][width][0];
        if (U_SUCCESS(errorCode) && patterns[index] == NULL) {
            if (minPlaceholders >= 0) {
                patterns[index] = new SimpleFormatter(
                        value.getUnicodeString(errorCode), minPlaceholders, 1, errorCode);
            }
            if (U_SUCCESS(errorCode) && patterns[index] == NULL) {
                errorCode = U_MEMORY_ALLOCATION_ERROR;
            }
        }
    }

    void setDnamIfAbsent(const ResourceValue &value, UErrorCode& errorCode) {
        U_ASSERT(unitIndex < MEAS_UNIT_COUNT);
        U_ASSERT(width < WIDTH_INDEX_COUNT);
        if (cacheData.dnams[unitIndex][width] == NULL) {
            int32_t length;
            cacheData.dnams[unitIndex][width] = value.getString(length, errorCode);
        }
    }

    /**
     * Consume a display pattern. For example,
     * unitsShort/duration/hour contains other{"{0} hrs"}.
     */
    void consumePattern(const char *key, const ResourceValue &value, UErrorCode &errorCode) {
        if (U_FAILURE(errorCode)) { return; }
        if (uprv_strcmp(key, "dnam") == 0) {
            // The display name for the unit in the current width.
            setDnamIfAbsent(value, errorCode);
        } else if (uprv_strcmp(key, "per") == 0) {
            // For example, "{0}/h".
            setFormatterIfAbsent(PER_UNIT_INDEX, value, 1, errorCode);
        } else {
            // The key must be one of the plural form strings. For example:
            // one{"{0} hr"}
            // other{"{0} hrs"}
            setFormatterIfAbsent(StandardPlural::indexFromString(key, errorCode), value, 0,
                                    errorCode);
        }
    }

    /**
     * Consume a table of per-unit tables. For example,
     * unitsShort/duration contains tables for duration-unit subtypes day & hour.
     */
    void consumeSubtypeTable(const char *key, ResourceValue &value, UErrorCode &errorCode) {
        if (U_FAILURE(errorCode)) { return; }
        unitIndex = MeasureUnit::internalGetIndexForTypeAndSubtype(type, key);
        if (unitIndex < 0) {
            // TODO: How to handle unexpected data?
            // See http://bugs.icu-project.org/trac/ticket/12597
            return;
        }

        // We no longer handle units like "coordinate" here (which do not have plural variants)
        if (value.getType() == URES_TABLE) {
            // Units that have plural variants
            ResourceTable patternTableTable = value.getTable(errorCode);
            if (U_FAILURE(errorCode)) { return; }
            for (int i = 0; patternTableTable.getKeyAndValue(i, key, value); ++i) {
                consumePattern(key, value, errorCode);
            }
        } else {
            // TODO: How to handle unexpected data?
            // See http://bugs.icu-project.org/trac/ticket/12597
            return;
        }
    }

    /**
     * Consume compound x-per-y display pattern. For example,
     * unitsShort/compound/per may be "{0}/{1}".
     */
    void consumeCompoundPattern(const char *key, const ResourceValue &value, UErrorCode &errorCode) {
        if (U_SUCCESS(errorCode) && uprv_strcmp(key, "per") == 0) {
            cacheData.perFormatters[width].
                    applyPatternMinMaxArguments(value.getUnicodeString(errorCode), 2, 2, errorCode);
        }
    }

    /**
     * Consume a table of unit type tables. For example,
     * unitsShort contains tables for area & duration.
     * It also contains a table for the compound/per pattern.
     */
    void consumeUnitTypesTable(const char *key, ResourceValue &value, UErrorCode &errorCode) {
        if (U_FAILURE(errorCode)) { return; }
        if (uprv_strcmp(key, "currency") == 0) {
            // Skip.
        } else if (uprv_strcmp(key, "compound") == 0) {
            if (!cacheData.hasPerFormatter(width)) {
                ResourceTable compoundTable = value.getTable(errorCode);
                if (U_FAILURE(errorCode)) { return; }
                for (int i = 0; compoundTable.getKeyAndValue(i, key, value); ++i) {
                    consumeCompoundPattern(key, value, errorCode);
                }
            }
        } else if (uprv_strcmp(key, "coordinate") == 0) {
            // special handling but we need to determine what that is
        } else {
            type = key;
            ResourceTable subtypeTable = value.getTable(errorCode);
            if (U_FAILURE(errorCode)) { return; }
            for (int i = 0; subtypeTable.getKeyAndValue(i, key, value); ++i) {
                consumeSubtypeTable(key, value, errorCode);
            }
        }
    }

    void consumeAlias(const char *key, const ResourceValue &value, UErrorCode &errorCode) {
        // Handle aliases like
        // units:alias{"/LOCALE/unitsShort"}
        // which should only occur in the root bundle.
        UMeasureFormatWidth sourceWidth = widthFromKey(key);
        if (sourceWidth == UMEASFMT_WIDTH_COUNT) {
            // Alias from something we don't care about.
            return;
        }
        UMeasureFormatWidth targetWidth = widthFromAlias(value, errorCode);
        if (targetWidth == UMEASFMT_WIDTH_COUNT) {
            // We do not recognize what to fall back to.
            errorCode = U_INVALID_FORMAT_ERROR;
            return;
        }
        // Check that we do not fall back to another fallback.
        if (cacheData.widthFallback[targetWidth] != UMEASFMT_WIDTH_COUNT) {
            errorCode = U_INVALID_FORMAT_ERROR;
            return;
        }
        cacheData.widthFallback[sourceWidth] = targetWidth;
    }

    void consumeTable(const char *key, ResourceValue &value, UErrorCode &errorCode) {
        if (U_SUCCESS(errorCode) && (width = widthFromKey(key)) != UMEASFMT_WIDTH_COUNT) {
            ResourceTable unitTypesTable = value.getTable(errorCode);
            if (U_FAILURE(errorCode)) { return; }
            for (int i = 0; unitTypesTable.getKeyAndValue(i, key, value); ++i) {
                consumeUnitTypesTable(key, value, errorCode);
            }
        }
    }

    static UMeasureFormatWidth widthFromKey(const char *key) {
        if (uprv_strncmp(key, "units", 5) == 0) {
            key += 5;
            if (*key == 0) {
                return UMEASFMT_WIDTH_WIDE;
            } else if (uprv_strcmp(key, "Short") == 0) {
                return UMEASFMT_WIDTH_SHORT;
            } else if (uprv_strcmp(key, "Narrow") == 0) {
                return UMEASFMT_WIDTH_NARROW;
            }
        }
        return UMEASFMT_WIDTH_COUNT;
    }

    static UMeasureFormatWidth widthFromAlias(const ResourceValue &value, UErrorCode &errorCode) {
        int32_t length;
        const UChar *s = value.getAliasString(length, errorCode);
        // For example: "/LOCALE/unitsShort"
        if (U_SUCCESS(errorCode) && length >= 13 && u_memcmp(s, g_LOCALE_units, 13) == 0) {
            s += 13;
            length -= 13;
            if (*s == 0) {
                return UMEASFMT_WIDTH_WIDE;
            } else if (u_strCompare(s, length, gShort, 5, FALSE) == 0) {
                return UMEASFMT_WIDTH_SHORT;
            } else if (u_strCompare(s, length, gNarrow, 6, FALSE) == 0) {
                return UMEASFMT_WIDTH_NARROW;
            }
        }
        return UMEASFMT_WIDTH_COUNT;
    }

    virtual void put(const char *key, ResourceValue &value, UBool /*noFallback*/,
            UErrorCode &errorCode) {
        // Main entry point to sink
        ResourceTable widthsTable = value.getTable(errorCode);
        if (U_FAILURE(errorCode)) { return; }
        for (int i = 0; widthsTable.getKeyAndValue(i, key, value); ++i) {
            if (value.getType() == URES_ALIAS) {
                consumeAlias(key, value, errorCode);
            } else {
                consumeTable(key, value, errorCode);
            }
        }
    }
};

// Virtual destructors must be defined out of line.
UnitDataSink::~UnitDataSink() {}

}  // namespace

static const UAMeasureUnit indexToUAMsasUnit[] = {
    // UAMeasureUnit                                 // UAMeasUnit vals  # MeasUnit.getIndex()
    UAMEASUNIT_ACCELERATION_G_FORCE,                 // (0 << 8) + 0,    #    0
    UAMEASUNIT_ACCELERATION_METER_PER_SECOND_SQUARED, // (0 << 8) + 1,   #    1
    UAMEASUNIT_ANGLE_ARC_MINUTE,                     // (1 << 8) + 1,    #    2
    UAMEASUNIT_ANGLE_ARC_SECOND,                     // (1 << 8) + 2,    #    3
    UAMEASUNIT_ANGLE_DEGREE,                         // (1 << 8) + 0,    #    4
    UAMEASUNIT_ANGLE_RADIAN,                         // (1 << 8) + 3,    #    5
    UAMEASUNIT_ANGLE_REVOLUTION,                     // (1 << 8) + 4,    #    6
    UAMEASUNIT_AREA_ACRE,                            // (2 << 8) + 4,    #    7
    UAMEASUNIT_AREA_HECTARE,                         // (2 << 8) + 5,    #    8
    UAMEASUNIT_AREA_SQUARE_CENTIMETER,               // (2 << 8) + 6,    #    9
    UAMEASUNIT_AREA_SQUARE_FOOT,                     // (2 << 8) + 2,    #    10
    UAMEASUNIT_AREA_SQUARE_INCH,                     // (2 << 8) + 7,    #    11
    UAMEASUNIT_AREA_SQUARE_KILOMETER,                // (2 << 8) + 1,    #    12
    UAMEASUNIT_AREA_SQUARE_METER,                    // (2 << 8) + 0,    #    13
    UAMEASUNIT_AREA_SQUARE_MILE,                     // (2 << 8) + 3,    #    14
    UAMEASUNIT_AREA_SQUARE_YARD,                     // (2 << 8) + 8,    #    15
    UAMEASUNIT_CONCENTRATION_KARAT,                  // (18 << 8) + 0,   #    16
    UAMEASUNIT_CONCENTRATION_MILLIGRAM_PER_DECILITER, // (18 << 8) + 1,  #    17
    UAMEASUNIT_CONCENTRATION_MILLIMOLE_PER_LITER,    // (18 << 8) + 2,   #    18
    UAMEASUNIT_CONCENTRATION_PART_PER_MILLION,       // (18 << 8) + 3,   #    19
    UAMEASUNIT_CONSUMPTION_LITER_PER_100_KILOMETERs, // (13 << 8) + 2,   #    20
    UAMEASUNIT_CONSUMPTION_LITER_PER_KILOMETER,      // (13 << 8) + 0,   #    21
    UAMEASUNIT_CONSUMPTION_MILE_PER_GALLON,          // (13 << 8) + 1,   #    22
    UAMEASUNIT_CONSUMPTION_MILE_PER_GALLON_IMPERIAL, // (13 << 8) + 3,   #    23
    UAMEASUNIT_DIGITAL_BIT,                          // (14 << 8) + 0,   #    24
    UAMEASUNIT_DIGITAL_BYTE,                         // (14 << 8) + 1,   #    25
    UAMEASUNIT_DIGITAL_GIGABIT,                      // (14 << 8) + 2,   #    26
    UAMEASUNIT_DIGITAL_GIGABYTE,                     // (14 << 8) + 3,   #    27
    UAMEASUNIT_DIGITAL_KILOBIT,                      // (14 << 8) + 4,   #    28
    UAMEASUNIT_DIGITAL_KILOBYTE,                     // (14 << 8) + 5,   #    29
    UAMEASUNIT_DIGITAL_MEGABIT,                      // (14 << 8) + 6,   #    30
    UAMEASUNIT_DIGITAL_MEGABYTE,                     // (14 << 8) + 7,   #    31
    UAMEASUNIT_DIGITAL_TERABIT,                      // (14 << 8) + 8,   #    32
    UAMEASUNIT_DIGITAL_TERABYTE,                     // (14 << 8) + 9,   #    33
    UAMEASUNIT_DURATION_CENTURY,                     // (4 << 8) + 10,   #    34
    UAMEASUNIT_DURATION_DAY,                         // (4 << 8) + 3,    #    35
    UAMEASUNIT_DURATION_HOUR,                        // (4 << 8) + 4,    #    36
    UAMEASUNIT_DURATION_MICROSECOND,                 // (4 << 8) + 8,    #    37
    UAMEASUNIT_DURATION_MILLISECOND,                 // (4 << 8) + 7,    #    38
    UAMEASUNIT_DURATION_MINUTE,                      // (4 << 8) + 5,    #    39
    UAMEASUNIT_DURATION_MONTH,                       // (4 << 8) + 1,    #    40
    UAMEASUNIT_DURATION_NANOSECOND,                  // (4 << 8) + 9,    #    41
    UAMEASUNIT_DURATION_SECOND,                      // (4 << 8) + 6,    #    42
    UAMEASUNIT_DURATION_WEEK,                        // (4 << 8) + 2,    #    43
    UAMEASUNIT_DURATION_YEAR,                        // (4 << 8) + 0,    #    44
    UAMEASUNIT_ELECTRIC_AMPERE,                      // (15 << 8) + 0,   #    45
    UAMEASUNIT_ELECTRIC_MILLIAMPERE,                 // (15 << 8) + 1,   #    46
    UAMEASUNIT_ELECTRIC_OHM,                         // (15 << 8) + 2,   #    47
    UAMEASUNIT_ELECTRIC_VOLT,                        // (15 << 8) + 3,   #    48
    UAMEASUNIT_ENERGY_CALORIE,                       // (12 << 8) + 0,   #    49
    UAMEASUNIT_ENERGY_FOODCALORIE,                   // (12 << 8) + 1,   #    50
    UAMEASUNIT_ENERGY_JOULE,                         // (12 << 8) + 2,   #    51
    UAMEASUNIT_ENERGY_KILOCALORIE,                   // (12 << 8) + 3,   #    52
    UAMEASUNIT_ENERGY_KILOJOULE,                     // (12 << 8) + 4,   #    53
    UAMEASUNIT_ENERGY_KILOWATT_HOUR,                 // (12 << 8) + 5,   #    54
    UAMEASUNIT_FREQUENCY_GIGAHERTZ,                  // (16 << 8) + 3,   #    55
    UAMEASUNIT_FREQUENCY_HERTZ,                      // (16 << 8) + 0,   #    56
    UAMEASUNIT_FREQUENCY_KILOHERTZ,                  // (16 << 8) + 1,   #    57
    UAMEASUNIT_FREQUENCY_MEGAHERTZ,                  // (16 << 8) + 2,   #    58
    UAMEASUNIT_LENGTH_ASTRONOMICAL_UNIT,             // (5 << 8) + 16,   #    59
    UAMEASUNIT_LENGTH_CENTIMETER,                    // (5 << 8) + 1,    #    60
    UAMEASUNIT_LENGTH_DECIMETER,                     // (5 << 8) + 10,   #    61
    UAMEASUNIT_LENGTH_FATHOM,                        // (5 << 8) + 14,   #    62
    UAMEASUNIT_LENGTH_FOOT,                          // (5 << 8) + 5,    #    63
    UAMEASUNIT_LENGTH_FURLONG,                       // (5 << 8) + 15,   #    64
    UAMEASUNIT_LENGTH_INCH,                          // (5 << 8) + 6,    #    65
    UAMEASUNIT_LENGTH_KILOMETER,                     // (5 << 8) + 2,    #    66
    UAMEASUNIT_LENGTH_LIGHT_YEAR,                    // (5 << 8) + 9,    #    67
    UAMEASUNIT_LENGTH_METER,                         // (5 << 8) + 0,    #    68
    UAMEASUNIT_LENGTH_MICROMETER,                    // (5 << 8) + 11,   #    69
    UAMEASUNIT_LENGTH_MILE,                          // (5 << 8) + 7,    #    70
    UAMEASUNIT_LENGTH_MILE_SCANDINAVIAN,             // (5 << 8) + 18,   #    71
    UAMEASUNIT_LENGTH_MILLIMETER,                    // (5 << 8) + 3,    #    72
    UAMEASUNIT_LENGTH_NANOMETER,                     // (5 << 8) + 12,   #    73
    UAMEASUNIT_LENGTH_NAUTICAL_MILE,                 // (5 << 8) + 13,   #    74
    UAMEASUNIT_LENGTH_PARSEC,                        // (5 << 8) + 17,   #    75
    UAMEASUNIT_LENGTH_PICOMETER,                     // (5 << 8) + 4,    #    76
    UAMEASUNIT_LENGTH_POINT,                         // (5 << 8) + 19,   #    77
    UAMEASUNIT_LENGTH_YARD,                          // (5 << 8) + 8,    #    78
    UAMEASUNIT_LIGHT_LUX,                            // (17 << 8) + 0,   #    79
    UAMEASUNIT_MASS_CARAT,                           // (6 << 8) + 9,    #    80
    UAMEASUNIT_MASS_GRAM,                            // (6 << 8) + 0,    #    81
    UAMEASUNIT_MASS_KILOGRAM,                        // (6 << 8) + 1,    #    82
    UAMEASUNIT_MASS_METRIC_TON,                      // (6 << 8) + 7,    #    83
    UAMEASUNIT_MASS_MICROGRAM,                       // (6 << 8) + 5,    #    84
    UAMEASUNIT_MASS_MILLIGRAM,                       // (6 << 8) + 6,    #    85
    UAMEASUNIT_MASS_OUNCE,                           // (6 << 8) + 2,    #    86
    UAMEASUNIT_MASS_OUNCE_TROY,                      // (6 << 8) + 10,   #    87
    UAMEASUNIT_MASS_POUND,                           // (6 << 8) + 3,    #    88
    UAMEASUNIT_MASS_STONE,                           // (6 << 8) + 4,    #    89
    UAMEASUNIT_MASS_TON,                             // (6 << 8) + 8,    #    90
    UAMEASUNIT_POWER_GIGAWATT,                       // (7 << 8) + 5,    #    91
    UAMEASUNIT_POWER_HORSEPOWER,                     // (7 << 8) + 2,    #    92
    UAMEASUNIT_POWER_KILOWATT,                       // (7 << 8) + 1,    #    93
    UAMEASUNIT_POWER_MEGAWATT,                       // (7 << 8) + 4,    #    94
    UAMEASUNIT_POWER_MILLIWATT,                      // (7 << 8) + 3,    #    95
    UAMEASUNIT_POWER_WATT,                           // (7 << 8) + 0,    #    96
    UAMEASUNIT_PRESSURE_HECTOPASCAL,                 // (8 << 8) + 0,    #    97
    UAMEASUNIT_PRESSURE_INCH_HG,                     // (8 << 8) + 1,    #    98
    UAMEASUNIT_PRESSURE_MILLIBAR,                    // (8 << 8) + 2,    #    99
    UAMEASUNIT_PRESSURE_MILLIMETER_OF_MERCURY,       // (8 << 8) + 3,    #    100
    UAMEASUNIT_PRESSURE_POUND_PER_SQUARE_INCH,       // (8 << 8) + 4,    #    101
    UAMEASUNIT_SPEED_KILOMETER_PER_HOUR,             // (9 << 8) + 1,    #    102
    UAMEASUNIT_SPEED_KNOT,                           // (9 << 8) + 3,    #    103
    UAMEASUNIT_SPEED_METER_PER_SECOND,               // (9 << 8) + 0,    #    104
    UAMEASUNIT_SPEED_MILE_PER_HOUR,                  // (9 << 8) + 2,    #    105
    UAMEASUNIT_TEMPERATURE_CELSIUS,                  // (10 << 8) + 0,   #    106
    UAMEASUNIT_TEMPERATURE_FAHRENHEIT,               // (10 << 8) + 1,   #    107
    UAMEASUNIT_TEMPERATURE_GENERIC,                  // (10 << 8) + 3,   #    108
    UAMEASUNIT_TEMPERATURE_KELVIN,                   // (10 << 8) + 2,   #    109
    UAMEASUNIT_VOLUME_ACRE_FOOT,                     // (11 << 8) + 13,  #    110
    UAMEASUNIT_VOLUME_BUSHEL,                        // (11 << 8) + 14,  #    111
    UAMEASUNIT_VOLUME_CENTILITER,                    // (11 << 8) + 4,   #    112
    UAMEASUNIT_VOLUME_CUBIC_CENTIMETER,              // (11 << 8) + 8,   #    113
    UAMEASUNIT_VOLUME_CUBIC_FOOT,                    // (11 << 8) + 11,  #    114
    UAMEASUNIT_VOLUME_CUBIC_INCH,                    // (11 << 8) + 10,  #    115
    UAMEASUNIT_VOLUME_CUBIC_KILOMETER,               // (11 << 8) + 1,   #    116
    UAMEASUNIT_VOLUME_CUBIC_METER,                   // (11 << 8) + 9,   #    117
    UAMEASUNIT_VOLUME_CUBIC_MILE,                    // (11 << 8) + 2,   #    118
    UAMEASUNIT_VOLUME_CUBIC_YARD,                    // (11 << 8) + 12,  #    119
    UAMEASUNIT_VOLUME_CUP,                           // (11 << 8) + 18,  #    120
    UAMEASUNIT_VOLUME_CUP_METRIC,                    // (11 << 8) + 22,  #    121
    UAMEASUNIT_VOLUME_DECILITER,                     // (11 << 8) + 5,   #    122
    UAMEASUNIT_VOLUME_FLUID_OUNCE,                   // (11 << 8) + 17,  #    123
    UAMEASUNIT_VOLUME_GALLON,                        // (11 << 8) + 21,  #    124
    UAMEASUNIT_VOLUME_GALLON_IMPERIAL,               // (11 << 8) + 24,  #    125
    UAMEASUNIT_VOLUME_HECTOLITER,                    // (11 << 8) + 6,   #    126
    UAMEASUNIT_VOLUME_LITER,                         // (11 << 8) + 0,   #    127
    UAMEASUNIT_VOLUME_MEGALITER,                     // (11 << 8) + 7,   #    128
    UAMEASUNIT_VOLUME_MILLILITER,                    // (11 << 8) + 3,   #    129
    UAMEASUNIT_VOLUME_PINT,                          // (11 << 8) + 19,  #    130
    UAMEASUNIT_VOLUME_PINT_METRIC,                   // (11 << 8) + 23,  #    131
    UAMEASUNIT_VOLUME_QUART,                         // (11 << 8) + 20,  #    132
    UAMEASUNIT_VOLUME_TABLESPOON,                    // (11 << 8) + 16,  #    133
    UAMEASUNIT_VOLUME_TEASPOON,                      // (11 << 8) + 15,  #    134
};

static UBool loadMeasureUnitData(
        const UResourceBundle *resource,
        MeasureFormatCacheData &cacheData,
        UErrorCode &status) {
    UnitDataSink sink(cacheData);
    ures_getAllItemsWithFallback(resource, "", sink, status);
    return U_SUCCESS(status);
}

static UnicodeString loadNumericDateFormatterPattern(
        const UResourceBundle *resource,
        const char *pattern,
        UErrorCode &status) {
    UnicodeString result;
    if (U_FAILURE(status)) {
        return result;
    }
    CharString chs;
    chs.append("durationUnits", status)
            .append("/", status).append(pattern, status);
    LocalUResourceBundlePointer patternBundle(
            ures_getByKeyWithFallback(
                resource,
                chs.data(),
                NULL,
                &status));
    if (U_FAILURE(status)) {
        return result;
    }
    getString(patternBundle.getAlias(), result, status);
    // Replace 'h' with 'H'
    int32_t len = result.length();
    UChar *buffer = result.getBuffer(len);
    for (int32_t i = 0; i < len; ++i) {
        if (buffer[i] == 0x68) { // 'h'
            buffer[i] = 0x48; // 'H'
        }
    }
    result.releaseBuffer(len);
    return result;
}

static NumericDateFormatters *loadNumericDateFormatters(
        const UResourceBundle *resource,
        UErrorCode &status) {
    if (U_FAILURE(status)) {
        return NULL;
    }
    NumericDateFormatters *result = new NumericDateFormatters(
        loadNumericDateFormatterPattern(resource, "hm", status),
        loadNumericDateFormatterPattern(resource, "ms", status),
        loadNumericDateFormatterPattern(resource, "hms", status),
        status);
    if (U_FAILURE(status)) {
        delete result;
        return NULL;
    }
    return result;
}

template<> U_I18N_API
const MeasureFormatCacheData *LocaleCacheKey<MeasureFormatCacheData>::createObject(
        const void * /*unused*/, UErrorCode &status) const {
    const char *localeId = fLoc.getName();
    LocalUResourceBundlePointer unitsBundle(ures_open(U_ICUDATA_UNIT, localeId, &status));
    static UNumberFormatStyle currencyStyles[] = {
            UNUM_CURRENCY_PLURAL, UNUM_CURRENCY_ISO, UNUM_CURRENCY};
    LocalPointer<MeasureFormatCacheData> result(new MeasureFormatCacheData(), status);
    if (U_FAILURE(status)) {
        return NULL;
    }
    if (!loadMeasureUnitData(
            unitsBundle.getAlias(),
            *result,
            status)) {
        return NULL;
    }
    result->adoptNumericDateFormatters(loadNumericDateFormatters(
            unitsBundle.getAlias(), status));
    if (U_FAILURE(status)) {
        return NULL;
    }

    for (int32_t i = 0; i < WIDTH_INDEX_COUNT; ++i) {
        // NumberFormat::createInstance can erase warning codes from status, so pass it
        // a separate status instance
        UErrorCode localStatus = U_ZERO_ERROR;
        result->adoptCurrencyFormat(i, NumberFormat::createInstance(
                localeId, currencyStyles[i], localStatus));
        if (localStatus != U_ZERO_ERROR) {
            status = localStatus;
        }
        if (U_FAILURE(status)) {
            return NULL;
        }
    }
    NumberFormat *inf = NumberFormat::createInstance(
            localeId, UNUM_DECIMAL, status);
    if (U_FAILURE(status)) {
        return NULL;
    }
    inf->setMaximumFractionDigits(0);
    DecimalFormat *decfmt = dynamic_cast<DecimalFormat *>(inf);
    if (decfmt != NULL) {
        decfmt->setRoundingMode(DecimalFormat::kRoundDown);
    }
    result->adoptIntegerFormat(inf);
    result->addRef();
    return result.orphan();
}

static UBool isTimeUnit(const MeasureUnit &mu, const char *tu) {
    return uprv_strcmp(mu.getType(), "duration") == 0 &&
            uprv_strcmp(mu.getSubtype(), tu) == 0;
}

// Converts a composite measure into hours-minutes-seconds and stores at hms
// array. [0] is hours; [1] is minutes; [2] is seconds. Returns a bit map of
// units found: 1=hours, 2=minutes, 4=seconds. For example, if measures
// contains hours-minutes, this function would return 3.
//
// If measures cannot be converted into hours, minutes, seconds or if amounts
// are negative, or if hours, minutes, seconds are out of order, returns 0.
static int32_t toHMS(
        const Measure *measures,
        int32_t measureCount,
        Formattable *hms,
        UErrorCode &status) {
    if (U_FAILURE(status)) {
        return 0;
    }
    int32_t result = 0;
    if (U_FAILURE(status)) {
        return 0;
    }
    // We use copy constructor to ensure that both sides of equality operator
    // are instances of MeasureUnit base class and not a subclass. Otherwise,
    // operator== will immediately return false.
    for (int32_t i = 0; i < measureCount; ++i) {
        if (isTimeUnit(measures[i].getUnit(), "hour")) {
            // hour must come first
            if (result >= 1) {
                return 0;
            }
            hms[0] = measures[i].getNumber();
            if (hms[0].getDouble() < 0.0) {
                return 0;
            }
            result |= 1;
        } else if (isTimeUnit(measures[i].getUnit(), "minute")) {
            // minute must come after hour
            if (result >= 2) {
                return 0;
            }
            hms[1] = measures[i].getNumber();
            if (hms[1].getDouble() < 0.0) {
                return 0;
            }
            result |= 2;
        } else if (isTimeUnit(measures[i].getUnit(), "second")) {
            // second must come after hour and minute
            if (result >= 4) {
                return 0;
            }
            hms[2] = measures[i].getNumber();
            if (hms[2].getDouble() < 0.0) {
                return 0;
            }
            result |= 4;
        } else {
            return 0;
        }
    }
    return result;
}


MeasureFormat::MeasureFormat(
        const Locale &locale, UMeasureFormatWidth w, UErrorCode &status)
        : cache(NULL),
          numberFormat(NULL),
          pluralRules(NULL),
          width((w==UMEASFMT_WIDTH_SHORTER)? UMEASFMT_WIDTH_SHORT: w),
          stripPatternSpaces(w==UMEASFMT_WIDTH_SHORTER),
          listFormatter(NULL),
          listFormatterStd(NULL) {
    initMeasureFormat(locale, (w==UMEASFMT_WIDTH_SHORTER)? UMEASFMT_WIDTH_SHORT: w, NULL, status);
}

MeasureFormat::MeasureFormat(
        const Locale &locale,
        UMeasureFormatWidth w,
        NumberFormat *nfToAdopt,
        UErrorCode &status) 
        : cache(NULL),
          numberFormat(NULL),
          pluralRules(NULL),
          width((w==UMEASFMT_WIDTH_SHORTER)? UMEASFMT_WIDTH_SHORT: w),
          stripPatternSpaces(w==UMEASFMT_WIDTH_SHORTER),
          listFormatter(NULL),
          listFormatterStd(NULL) {
    initMeasureFormat(locale, (w==UMEASFMT_WIDTH_SHORTER)? UMEASFMT_WIDTH_SHORT: w, nfToAdopt, status);
}

MeasureFormat::MeasureFormat(const MeasureFormat &other) :
        Format(other),
        cache(other.cache),
        numberFormat(other.numberFormat),
        pluralRules(other.pluralRules),
        width(other.width),
        stripPatternSpaces(other.stripPatternSpaces),
        listFormatter(NULL),
        listFormatterStd(NULL) {
    cache->addRef();
    numberFormat->addRef();
    pluralRules->addRef();
    if (other.listFormatter != NULL) {
        listFormatter = new ListFormatter(*other.listFormatter);
    }
    if (other.listFormatterStd != NULL) {
        listFormatterStd = new ListFormatter(*other.listFormatterStd);
    }
}

MeasureFormat &MeasureFormat::operator=(const MeasureFormat &other) {
    if (this == &other) {
        return *this;
    }
    Format::operator=(other);
    SharedObject::copyPtr(other.cache, cache);
    SharedObject::copyPtr(other.numberFormat, numberFormat);
    SharedObject::copyPtr(other.pluralRules, pluralRules);
    width = other.width;
    stripPatternSpaces = other.stripPatternSpaces;
    delete listFormatter;
    if (other.listFormatter != NULL) {
        listFormatter = new ListFormatter(*other.listFormatter);
    } else {
        listFormatter = NULL;
    }
    delete listFormatterStd;
    if (other.listFormatterStd != NULL) {
        listFormatterStd = new ListFormatter(*other.listFormatterStd);
    } else {
        listFormatterStd = NULL;
    }
    return *this;
}

MeasureFormat::MeasureFormat() :
        cache(NULL),
        numberFormat(NULL),
        pluralRules(NULL),
        width(UMEASFMT_WIDTH_SHORT),
        stripPatternSpaces(FALSE),
        listFormatter(NULL),
        listFormatterStd(NULL) {
}

MeasureFormat::~MeasureFormat() {
    if (cache != NULL) {
        cache->removeRef();
    }
    if (numberFormat != NULL) {
        numberFormat->removeRef();
    }
    if (pluralRules != NULL) {
        pluralRules->removeRef();
    }
    delete listFormatter;
    delete listFormatterStd;
}

UBool MeasureFormat::operator==(const Format &other) const {
    if (this == &other) { // Same object, equal
        return TRUE;
    }
    if (!Format::operator==(other)) {
        return FALSE;
    }
    const MeasureFormat &rhs = static_cast<const MeasureFormat &>(other);

    // Note: Since the ListFormatter depends only on Locale and width, we
    // don't have to check it here.

    // differing widths aren't equivalent
    if (width != rhs.width || stripPatternSpaces != rhs.stripPatternSpaces) {
        return FALSE;
    }
    // Width the same check locales.
    // We don't need to check locales if both objects have same cache.
    if (cache != rhs.cache) {
        UErrorCode status = U_ZERO_ERROR;
        const char *localeId = getLocaleID(status);
        const char *rhsLocaleId = rhs.getLocaleID(status);
        if (U_FAILURE(status)) {
            // On failure, assume not equal
            return FALSE;
        }
        if (uprv_strcmp(localeId, rhsLocaleId) != 0) {
            return FALSE;
        }
    }
    // Locales same, check NumberFormat if shared data differs.
    return (
            numberFormat == rhs.numberFormat ||
            **numberFormat == **rhs.numberFormat);
}

Format *MeasureFormat::clone() const {
    return new MeasureFormat(*this);
}

UnicodeString &MeasureFormat::format(
        const Formattable &obj,
        UnicodeString &appendTo,
        FieldPosition &pos,
        UErrorCode &status) const {
    if (U_FAILURE(status)) return appendTo;
    if (obj.getType() == Formattable::kObject) {
        const UObject* formatObj = obj.getObject();
        const Measure* amount = dynamic_cast<const Measure*>(formatObj);
        if (amount != NULL) {
            return formatMeasure(
                    *amount, **numberFormat, appendTo, pos, status);
        }
    }
    status = U_ILLEGAL_ARGUMENT_ERROR;
    return appendTo;
}

void MeasureFormat::parseObject(
        const UnicodeString & /*source*/,
        Formattable & /*result*/,
        ParsePosition& /*pos*/) const {
    return;
}

UnicodeString &MeasureFormat::formatMeasurePerUnit(
        const Measure &measure,
        const MeasureUnit &perUnit,
        UnicodeString &appendTo,
        FieldPosition &pos,
        UErrorCode &status) const {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    bool isResolved = false;
    MeasureUnit resolvedUnit =
        MeasureUnit::resolveUnitPerUnit(measure.getUnit(), perUnit, &isResolved);
    if (isResolved) {
        Measure newMeasure(measure.getNumber(), new MeasureUnit(resolvedUnit), status);
        return formatMeasure(
                newMeasure, **numberFormat, appendTo, pos, status);
    }
    FieldPosition fpos(pos.getField());
    UnicodeString result;
    int32_t offset = withPerUnitAndAppend(
            formatMeasure(
                    measure, **numberFormat, result, fpos, status),
            perUnit,
            appendTo,
            status);
    if (U_FAILURE(status)) {
        return appendTo;
    }
    if (fpos.getBeginIndex() != 0 || fpos.getEndIndex() != 0) {
        pos.setBeginIndex(fpos.getBeginIndex() + offset);
        pos.setEndIndex(fpos.getEndIndex() + offset);
    }
    return appendTo;
}

UnicodeString &MeasureFormat::formatMeasures(
        const Measure *measures,
        int32_t measureCount,
        UnicodeString &appendTo,
        FieldPosition &pos,
        UErrorCode &status) const {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    if (measureCount == 0) {
        return appendTo;
    }
    if (measureCount == 1) {
        return formatMeasure(measures[0], **numberFormat, appendTo, pos, status);
    }
    if (width == UMEASFMT_WIDTH_NUMERIC) {
        Formattable hms[3];
        int32_t bitMap = toHMS(measures, measureCount, hms, status);
        if (bitMap > 0) {
            FieldPositionIteratorHandler handler(NULL, status);
            return formatNumeric(hms, bitMap, appendTo, handler, status);
        }
    }
    if (pos.getField() != FieldPosition::DONT_CARE) {
        return formatMeasuresSlowTrack(
                measures, measureCount, appendTo, pos, status);
    }
    UnicodeString *results = new UnicodeString[measureCount];
    if (results == NULL) {
        status = U_MEMORY_ALLOCATION_ERROR;
        return appendTo;
    }
    for (int32_t i = 0; i < measureCount; ++i) {
        const NumberFormat *nf = cache->getIntegerFormat();
        if (i == measureCount - 1) {
            nf = numberFormat->get();
        }
        formatMeasure(
                measures[i],
                *nf,
                results[i],
                pos,
                status);
    }
    listFormatter->format(results, measureCount, appendTo, status);
    delete [] results;
    return appendTo;
}

// Apple-specific version for now;
// uses FieldPositionIterator* instead of FieldPosition&
UnicodeString &MeasureFormat::formatMeasures(
        const Measure *measures,
        int32_t measureCount,
        UnicodeString &appendTo,
        FieldPositionIterator* posIter,
        UErrorCode &status) const {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    FieldPositionIteratorHandler handler(posIter, status);
    if (measureCount == 0) {
        return appendTo;
    }
    if (measureCount == 1) {
        int32_t start = appendTo.length();
        int32_t field = indexToUAMsasUnit[measures[0].getUnit().getIndex()];
        FieldPosition pos(UAMEASFMT_NUMERIC_FIELD_FLAG); // special field value to request range of entire numeric part
        formatMeasure(measures[0], **numberFormat, appendTo, pos, status);
        handler.addAttribute(field, start, appendTo.length());
        handler.addAttribute(field | UAMEASFMT_NUMERIC_FIELD_FLAG, pos.getBeginIndex(), pos.getEndIndex());
        return appendTo;
    }
    if (width == UMEASFMT_WIDTH_NUMERIC) {
        Formattable hms[3];
        int32_t bitMap = toHMS(measures, measureCount, hms, status);
        if (bitMap > 0) {
            return formatNumeric(hms, bitMap, appendTo, handler, status);
        }
    }
    UnicodeString *results = new UnicodeString[measureCount];
    if (results == NULL) {
        status = U_MEMORY_ALLOCATION_ERROR;
        return appendTo;
    }
    FieldPosition *numPositions = new FieldPosition[measureCount];
    if (results == NULL) {
        delete [] results;
        status = U_MEMORY_ALLOCATION_ERROR;
        return appendTo;
    }
    
    for (int32_t i = 0; i < measureCount; ++i) {
        const NumberFormat *nf = cache->getIntegerFormat();
        if (i == measureCount - 1) {
            nf = numberFormat->get();
        }
        numPositions[i].setField(UAMEASFMT_NUMERIC_FIELD_FLAG);
        formatMeasure(
                measures[i],
                *nf,
                results[i],
                numPositions[i],
                status);
    }
    listFormatter->format(results, measureCount, appendTo, status);
    for (int32_t i = 0; i < measureCount; ++i) {
        int32_t begin = appendTo.indexOf(results[i]);
        if (begin >= 0) {
            int32_t field = indexToUAMsasUnit[measures[i].getUnit().getIndex()];
            handler.addAttribute(field, begin, begin + results[i].length());
            int32_t numPosBegin = numPositions[i].getBeginIndex();
            int32_t numPosEnd   = numPositions[i].getEndIndex();
            if (numPosBegin >= 0 && numPosEnd > numPosBegin) {
                handler.addAttribute(field | UAMEASFMT_NUMERIC_FIELD_FLAG, begin + numPosBegin, begin + numPosEnd);
            }
        }
    }
    delete [] results;
    delete [] numPositions;
    return appendTo;
}


UnicodeString MeasureFormat::getUnitDisplayName(const MeasureUnit& unit, UErrorCode& /*status*/) const {
    UMeasureFormatWidth width = getRegularWidth(this->width);
    const UChar* const* styleToDnam = cache->dnams[unit.getIndex()];
    const UChar* dnam = styleToDnam[width];
    if (dnam == NULL) {
        int32_t fallbackWidth = cache->widthFallback[width];
        dnam = styleToDnam[fallbackWidth];
    }

    UnicodeString result;
    if (dnam == NULL) {
        result.setToBogus();
    } else {
        result.setTo(dnam, -1);
    }
    return result;
}

void MeasureFormat::initMeasureFormat(
        const Locale &locale,
        UMeasureFormatWidth w,
        NumberFormat *nfToAdopt,
        UErrorCode &status) {
    static const char *listStyles[] = {"unit", "unit-short", "unit-narrow"};
    LocalPointer<NumberFormat> nf(nfToAdopt);
    if (U_FAILURE(status)) {
        return;
    }
    const char *name = locale.getName();
    setLocaleIDs(name, name);

    UnifiedCache::getByLocale(locale, cache, status);
    if (U_FAILURE(status)) {
        return;
    }

    const SharedPluralRules *pr = PluralRules::createSharedInstance(
            locale, UPLURAL_TYPE_CARDINAL, status);
    if (U_FAILURE(status)) {
        return;
    }
    SharedObject::copyPtr(pr, pluralRules);
    pr->removeRef();
    if (nf.isNull()) {
        const SharedNumberFormat *shared = NumberFormat::createSharedInstance(
                locale, UNUM_DECIMAL, status);
        if (U_FAILURE(status)) {
            return;
        }
        SharedObject::copyPtr(shared, numberFormat);
        shared->removeRef();
    } else {
        adoptNumberFormat(nf.orphan(), status);
        if (U_FAILURE(status)) {
            return;
        }
    }
    width = w;
    if (stripPatternSpaces) {
        w = UMEASFMT_WIDTH_NARROW;
    }
    delete listFormatter;
    listFormatter = ListFormatter::createInstance(
            locale,
            listStyles[getRegularWidth(w)],
            status);
    delete listFormatterStd;
    listFormatterStd = ListFormatter::createInstance(
            locale,
            "standard",
            status);
}

void MeasureFormat::adoptNumberFormat(
        NumberFormat *nfToAdopt, UErrorCode &status) {
    LocalPointer<NumberFormat> nf(nfToAdopt);
    if (U_FAILURE(status)) {
        return;
    }
    SharedNumberFormat *shared = new SharedNumberFormat(nf.getAlias());
    if (shared == NULL) {
        status = U_MEMORY_ALLOCATION_ERROR;
        return;
    }
    nf.orphan();
    SharedObject::copyPtr(shared, numberFormat);
}

UBool MeasureFormat::setMeasureFormatLocale(const Locale &locale, UErrorCode &status) {
    if (U_FAILURE(status) || locale == getLocale(status)) {
        return FALSE;
    }
    initMeasureFormat(locale, width, NULL, status);
    return U_SUCCESS(status);
} 

// Apple-specific for now
UMeasureFormatWidth MeasureFormat::getWidth() const {
    return width;
}

const NumberFormat &MeasureFormat::getNumberFormat() const {
    return **numberFormat;
}

const PluralRules &MeasureFormat::getPluralRules() const {
    return **pluralRules;
}

Locale MeasureFormat::getLocale(UErrorCode &status) const {
    return Format::getLocale(ULOC_VALID_LOCALE, status);
}

const char *MeasureFormat::getLocaleID(UErrorCode &status) const {
    return Format::getLocaleID(ULOC_VALID_LOCALE, status);
}

// Apple=specific
// now just re-implement using standard getUnitDisplayName
// so we no longer use cache->getDisplayName
UnicodeString &MeasureFormat::getUnitName(
        const MeasureUnit* unit,
        UnicodeString &result ) const {
    UErrorCode status = U_ZERO_ERROR;
    result = getUnitDisplayName(*unit, status); // does not use or set status
    return result;
}

// Apple=specific
UnicodeString &MeasureFormat::getMultipleUnitNames(
        const MeasureUnit** units,
        int32_t unitCount,
        UAMeasureNameListStyle listStyle,
        UnicodeString &result ) const {
    if (unitCount == 0) {
        return result.remove();
    }
    if (unitCount == 1) {
        return getUnitName(units[0], result);
    }
    UnicodeString *results = new UnicodeString[unitCount];
    if (results != NULL) {
        for (int32_t i = 0; i < unitCount; ++i) {
            getUnitName(units[i], results[i]);
        }
        UErrorCode status = U_ZERO_ERROR;
        if (listStyle == UAMEASNAME_LIST_STANDARD) {
            listFormatterStd->format(results, unitCount, result, status);
        } else {
            listFormatter->format(results, unitCount, result, status);
        }
        delete [] results;
        if (U_SUCCESS(status)) {
            return result;
        }
    }
    result.setToBogus();
    return result;
}

UnicodeString &MeasureFormat::formatMeasure(
        const Measure &measure,
        const NumberFormat &nf,
        UnicodeString &appendTo,
        FieldPosition &pos,
        UErrorCode &status) const {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    const Formattable& amtNumber = measure.getNumber();
    const MeasureUnit& amtUnit = measure.getUnit();
    if (isCurrency(amtUnit)) {
        UChar isoCode[4];
        u_charsToUChars(amtUnit.getSubtype(), isoCode, 4);
        return cache->getCurrencyFormat(width)->format(
                new CurrencyAmount(amtNumber, isoCode, status),
                appendTo,
                pos,
                status);
    }
    UnicodeString formattedNumber;
    UBool posForFullNumericPart = (pos.getField() == UAMEASFMT_NUMERIC_FIELD_FLAG);
    if (posForFullNumericPart) {
        pos.setField(FieldPosition::DONT_CARE);
    }
    StandardPlural::Form pluralForm = QuantityFormatter::selectPlural(
            amtNumber, nf, **pluralRules, formattedNumber, pos, status);
    if (posForFullNumericPart) {
        pos.setField(UAMEASFMT_NUMERIC_FIELD_FLAG);
        pos.setBeginIndex(0);
        pos.setEndIndex(formattedNumber.length());
    }
    const SimpleFormatter *formatter = getPluralFormatter(amtUnit, width, pluralForm, status);
    int32_t cur = appendTo.length();
    QuantityFormatter::format(*formatter, formattedNumber, appendTo, pos, status);
    if (stripPatternSpaces) {
        const SimpleFormatter *narrowFormatter = getPluralFormatter(amtUnit, UMEASFMT_WIDTH_NARROW, pluralForm, status);
        if (U_SUCCESS(status)) {
            // Get the narrow pattern with all {n} set to empty string.
            // If there are spaces in that, then do not continue to strip spaces
            // (i.e. even in the narrowest form this locale keeps spaces).
            UnicodeString narrowPatternNoArgs = narrowFormatter->getTextWithNoArguments();
            if (narrowPatternNoArgs.indexOf((UChar)0x0020) == -1 && narrowPatternNoArgs.indexOf((UChar)0x00A0) == -1) {
                int32_t end = appendTo.length();
                for (; cur < end; cur++) {
                    if (appendTo.charAt(cur) == 0x0020) {
                        appendTo.remove(cur, 1);
                        if (pos.getBeginIndex() > cur) {
                            pos.setBeginIndex(pos.getBeginIndex() - 1);
                            pos.setEndIndex(pos.getEndIndex() - 1);
                        }
                    }
                }
            }
        }
    }
    return appendTo;
}

// Formats hours-minutes-seconds as 5:37:23 or similar.
UnicodeString &MeasureFormat::formatNumeric(
        const Formattable *hms,  // always length 3
        int32_t bitMap,   // 1=hourset, 2=minuteset, 4=secondset
        UnicodeString &appendTo,
        FieldPositionHandler& handler,
        UErrorCode &status) const {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    UDate millis = 
        (UDate) (((uprv_trunc(hms[0].getDouble(status)) * 60.0
             + uprv_trunc(hms[1].getDouble(status))) * 60.0
                  + uprv_trunc(hms[2].getDouble(status))) * 1000.0);
    switch (bitMap) {
    case 5: // hs
    case 7: // hms
        return formatNumeric(
                millis,
                cache->getNumericDateFormatters()->hourMinuteSecond,
                UDAT_SECOND_FIELD,
                hms[2],
                appendTo,
                handler,
                status);
        break;
    case 6: // ms
        return formatNumeric(
                millis,
                cache->getNumericDateFormatters()->minuteSecond,
                UDAT_SECOND_FIELD,
                hms[2],
                appendTo,
                handler,
                status);
        break;
    case 3: // hm
        return formatNumeric(
                millis,
                cache->getNumericDateFormatters()->hourMinute,
                UDAT_MINUTE_FIELD,
                hms[1],
                appendTo,
                handler,
                status);
        break;
    default:
        status = U_INTERNAL_PROGRAM_ERROR;
        return appendTo;
        break;
    }
    return appendTo;
}

static void appendRange(
        const UnicodeString &src,
        int32_t start,
        int32_t end,
        UnicodeString &dest) {
    dest.append(src, start, end - start);
}

static void appendRange(
        const UnicodeString &src,
        int32_t end,
        UnicodeString &dest) {
    dest.append(src, end, src.length() - end);
}

// Formats time like 5:37:23
UnicodeString &MeasureFormat::formatNumeric(
        UDate date, // Time since epoch 1:30:00 would be 5400000
        const DateFormat &dateFmt, // h:mm, m:ss, or h:mm:ss
        UDateFormatField smallestField, // seconds in 5:37:23.5
        const Formattable &smallestAmount, // 23.5 for 5:37:23.5
        UnicodeString &appendTo,
        FieldPositionHandler& handler,
        UErrorCode &status) const {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    // Format the smallest amount with this object's NumberFormat
    UnicodeString smallestAmountFormatted;

    // We keep track of the integer part of smallest amount so that
    // we can replace it later so that we get '0:00:09.3' instead of
    // '0:00:9.3'
    FieldPosition intFieldPosition(UNUM_INTEGER_FIELD);
    (*numberFormat)->format(
            smallestAmount, smallestAmountFormatted, intFieldPosition, status);
    if (
            intFieldPosition.getBeginIndex() == 0 &&
            intFieldPosition.getEndIndex() == 0) {
        status = U_INTERNAL_PROGRAM_ERROR;
        return appendTo;
    }

    // Format time. draft becomes something like '5:30:45'
    // #13606: DateFormat is not thread-safe, but MeasureFormat advertises itself as thread-safe.
    FieldPositionIterator posIter;
    UnicodeString draft;
    static UMutex dateFmtMutex = U_MUTEX_INITIALIZER;
    umtx_lock(&dateFmtMutex);
    dateFmt.format(date, draft, &posIter, status);
    umtx_unlock(&dateFmtMutex);

    int32_t start = appendTo.length();
    FieldPosition smallestFieldPosition(smallestField);
    FieldPosition fp;
    int32_t measField = -1;
    while (posIter.next(fp)) {
        int32_t dateField = fp.getField();
        switch (dateField) {
            case UDAT_HOUR_OF_DAY1_FIELD:
            case UDAT_HOUR_OF_DAY0_FIELD:
            case UDAT_HOUR1_FIELD:
            case UDAT_HOUR0_FIELD:
                measField = UAMEASUNIT_DURATION_HOUR; break;
            case UDAT_MINUTE_FIELD:
                measField = UAMEASUNIT_DURATION_MINUTE; break;
            case UDAT_SECOND_FIELD:
                measField = UAMEASUNIT_DURATION_SECOND; break;
            default:
                measField = -1; break;
        }
        if (dateField != smallestField) {
            if (measField >= 0) {
                handler.addAttribute(measField, start + fp.getBeginIndex(), start + fp.getEndIndex());
                handler.addAttribute(measField | UAMEASFMT_NUMERIC_FIELD_FLAG, start + fp.getBeginIndex(), start + fp.getEndIndex());
            }
        } else {
            smallestFieldPosition.setBeginIndex(fp.getBeginIndex());
            smallestFieldPosition.setEndIndex(fp.getEndIndex());
            break;
        }
    }

    // If we find field for smallest amount replace it with the formatted
    // smallest amount from above taking care to replace the integer part
    // with what is in original time. For example, If smallest amount
    // is 9.35s and the formatted time is 0:00:09 then 9.35 becomes 09.35
    // and replacing yields 0:00:09.35
    if (smallestFieldPosition.getBeginIndex() != 0 ||
            smallestFieldPosition.getEndIndex() != 0) {
        appendRange(draft, 0, smallestFieldPosition.getBeginIndex(), appendTo);
        appendRange(
                smallestAmountFormatted,
                0,
                intFieldPosition.getBeginIndex(),
                appendTo);
        appendRange(
                draft,
                smallestFieldPosition.getBeginIndex(),
                smallestFieldPosition.getEndIndex(),
                appendTo);
        appendRange(
                smallestAmountFormatted,
                intFieldPosition.getEndIndex(),
                appendTo);
        appendRange(
                draft,
                smallestFieldPosition.getEndIndex(),
                appendTo);
        handler.addAttribute(measField, start + smallestFieldPosition.getBeginIndex(), appendTo.length());
        handler.addAttribute(measField | UAMEASFMT_NUMERIC_FIELD_FLAG, start + smallestFieldPosition.getBeginIndex(), appendTo.length());
    } else {
        appendTo.append(draft);
    }
    return appendTo;
}

const SimpleFormatter *MeasureFormat::getFormatterOrNull(
        const MeasureUnit &unit, UMeasureFormatWidth width, int32_t index) const {
    width = getRegularWidth(width);
    SimpleFormatter *const (*unitPatterns)[PATTERN_COUNT] = &cache->patterns[unit.getIndex()][0];
    if (unitPatterns[width][index] != NULL) {
        return unitPatterns[width][index];
    }
    int32_t fallbackWidth = cache->widthFallback[width];
    if (fallbackWidth != UMEASFMT_WIDTH_COUNT && unitPatterns[fallbackWidth][index] != NULL) {
        return unitPatterns[fallbackWidth][index];
    }
    return NULL;
}

const SimpleFormatter *MeasureFormat::getFormatter(
        const MeasureUnit &unit, UMeasureFormatWidth width, int32_t index,
        UErrorCode &errorCode) const {
    if (U_FAILURE(errorCode)) {
        return NULL;
    }
    const SimpleFormatter *pattern = getFormatterOrNull(unit, width, index);
    if (pattern == NULL) {
        errorCode = U_MISSING_RESOURCE_ERROR;
    }
    return pattern;
}

const SimpleFormatter *MeasureFormat::getPluralFormatter(
        const MeasureUnit &unit, UMeasureFormatWidth width, int32_t index,
        UErrorCode &errorCode) const {
    if (U_FAILURE(errorCode)) {
        return NULL;
    }
    if (index != StandardPlural::OTHER) {
        const SimpleFormatter *pattern = getFormatterOrNull(unit, width, index);
        if (pattern != NULL) {
            return pattern;
        }
    }
    return getFormatter(unit, width, StandardPlural::OTHER, errorCode);
}

const SimpleFormatter *MeasureFormat::getPerFormatter(
        UMeasureFormatWidth width,
        UErrorCode &status) const {
    if (U_FAILURE(status)) {
        return NULL;
    }
    width = getRegularWidth(width);
    const SimpleFormatter * perFormatters = cache->perFormatters;
    if (perFormatters[width].getArgumentLimit() == 2) {
        return &perFormatters[width];
    }
    int32_t fallbackWidth = cache->widthFallback[width];
    if (fallbackWidth != UMEASFMT_WIDTH_COUNT &&
            perFormatters[fallbackWidth].getArgumentLimit() == 2) {
        return &perFormatters[fallbackWidth];
    }
    status = U_MISSING_RESOURCE_ERROR;
    return NULL;
}

int32_t MeasureFormat::withPerUnitAndAppend(
        const UnicodeString &formatted,
        const MeasureUnit &perUnit,
        UnicodeString &appendTo,
        UErrorCode &status) const {
    int32_t offset = -1;
    if (U_FAILURE(status)) {
        return offset;
    }
    const SimpleFormatter *perUnitFormatter = getFormatterOrNull(perUnit, width, PER_UNIT_INDEX);
    if (perUnitFormatter != NULL) {
        const UnicodeString *params[] = {&formatted};
        perUnitFormatter->formatAndAppend(
                params,
                UPRV_LENGTHOF(params),
                appendTo,
                &offset,
                1,
                status);
        return offset;
    }
    const SimpleFormatter *perFormatter = getPerFormatter(width, status);
    const SimpleFormatter *pattern =
            getPluralFormatter(perUnit, width, StandardPlural::ONE, status);
    if (U_FAILURE(status)) {
        return offset;
    }
    UnicodeString perUnitString = pattern->getTextWithNoArguments();
    perUnitString.trim();
    const UnicodeString *params[] = {&formatted, &perUnitString};
    perFormatter->formatAndAppend(
            params,
            UPRV_LENGTHOF(params),
            appendTo,
            &offset,
            1,
            status);
    return offset;
}

UnicodeString &MeasureFormat::formatMeasuresSlowTrack(
        const Measure *measures,
        int32_t measureCount,
        UnicodeString& appendTo,
        FieldPosition& pos,
        UErrorCode& status) const {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    FieldPosition dontCare(FieldPosition::DONT_CARE);
    FieldPosition fpos(pos.getField());
    UnicodeString *results = new UnicodeString[measureCount];
    int32_t fieldPositionFoundIndex = -1;
    for (int32_t i = 0; i < measureCount; ++i) {
        const NumberFormat *nf = cache->getIntegerFormat();
        if (i == measureCount - 1) {
            nf = numberFormat->get();
        }
        if (fieldPositionFoundIndex == -1) {
            formatMeasure(measures[i], *nf, results[i], fpos, status);
            if (U_FAILURE(status)) {
                delete [] results;
                return appendTo;
            }
            if (fpos.getBeginIndex() != 0 || fpos.getEndIndex() != 0) {
                fieldPositionFoundIndex = i;
            }
        } else {
            formatMeasure(measures[i], *nf, results[i], dontCare, status);
        }
    }
    int32_t offset;
    listFormatter->format(
            results,
            measureCount,
            appendTo,
            fieldPositionFoundIndex,
            offset,
            status);
    if (U_FAILURE(status)) {
        delete [] results;
        return appendTo;
    }
    if (offset != -1) {
        pos.setBeginIndex(fpos.getBeginIndex() + offset);
        pos.setEndIndex(fpos.getEndIndex() + offset);
    }
    delete [] results;
    return appendTo;
}

MeasureFormat* U_EXPORT2 MeasureFormat::createCurrencyFormat(const Locale& locale,
                                                   UErrorCode& ec) {
    CurrencyFormat* fmt = NULL;
    if (U_SUCCESS(ec)) {
        fmt = new CurrencyFormat(locale, ec);
        if (U_FAILURE(ec)) {
            delete fmt;
            fmt = NULL;
        }
    }
    return fmt;
}

MeasureFormat* U_EXPORT2 MeasureFormat::createCurrencyFormat(UErrorCode& ec) {
    if (U_FAILURE(ec)) {
        return NULL;
    }
    return MeasureFormat::createCurrencyFormat(Locale::getDefault(), ec);
}

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */
