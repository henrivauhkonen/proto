#ifndef DEBUG_LOGGING_H
#define DEBUG_LOGGING_H

/*
 * Yhteinen debug-loggausrajapinta mittausprojektia varten.
 *
 * Tavoitteet:
 * - Selkeä ja yhtenäinen loggausformaatti
 * - Käyttökelpoinen sekä STM32Cube/HAL- että STM32duino-portissa
 * - Helppo karsia pois compile-timessa
 * - Mahdollisuus käyttää sekä uusia LOG_* makroja että vanhaa
 *   debug_logging_printf()-rajapintaa siirtymävaiheessa
 *
 * Suunnitteluperiaate:
 * - Header tarjoaa koko projektin yhteisen julkisen rajapinnan
 * - Varsinainen loggausbackend toteutetaan erikseen debug_logging.cpp:ssä
 * - Korkean tason käyttö tapahtuu ensisijaisesti LOG_ERROR / LOG_WARN /
 *   LOG_INFO / LOG_DEBUG / LOG_TRACE -makroilla
 * - Vanha debug_logging_printf() pidetään mukana, jotta olemassa oleva
 *   koodi voidaan siirtää uuteen malliin asteittain eikä kerralla
 */

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Logitasojen numeeriset arvot
 * ============================================================================
 *
 * Näitä käytetään compile-time suodatuksessa.
 *
 * Mitä suurempi arvo, sitä puheliaampi loggaus:
 *
 *   ERROR = vain vakavat virheet
 *   WARN  = virheet + varoitukset
 *   INFO  = tilasiirtymät, korkean tason eteneminen
 *   DEBUG = tarkempi mittaus- ja tilatieto
 *   TRACE = kaikkein yksityiskohtaisin diagnostiikka
 *
 * Oletuksena projekti käynnistyy DEBUG-tasolla, mikä on hyvä porttaus-
 * ja bring-up-vaiheessa.
 */
#define DEBUG_LOG_SEVERITY_ERROR_VALUE  0
#define DEBUG_LOG_SEVERITY_WARN_VALUE   1
#define DEBUG_LOG_SEVERITY_INFO_VALUE   2
#define DEBUG_LOG_SEVERITY_DEBUG_VALUE  3
#define DEBUG_LOG_SEVERITY_TRACE_VALUE  4

/*
 * Tyyppiturvallinen enum logitasolle.
 *
 * Tätä käytetään varsinaisessa toteutusfunktiossa ja mahdollistaa
 * selkeästi luettavan logitason välittämisen ilman "taikalukuja".
 */
typedef enum
{
    DEBUG_LOG_SEVERITY_ERROR = DEBUG_LOG_SEVERITY_ERROR_VALUE,
    DEBUG_LOG_SEVERITY_WARN  = DEBUG_LOG_SEVERITY_WARN_VALUE,
    DEBUG_LOG_SEVERITY_INFO  = DEBUG_LOG_SEVERITY_INFO_VALUE,
    DEBUG_LOG_SEVERITY_DEBUG = DEBUG_LOG_SEVERITY_DEBUG_VALUE,
    DEBUG_LOG_SEVERITY_TRACE = DEBUG_LOG_SEVERITY_TRACE_VALUE
} debug_log_severity_t;

/*
 * ============================================================================
 * Koko loggausjärjestelmän compile-time asetukset
 * ============================================================================
 */

/*
 * Pääkatkaisin koko loggausjärjestelmälle.
 *
 * 1 = loggaus käytössä
 * 0 = loggaus kytketty kokonaan pois
 *
 * Kun tämä asetetaan nollaksi:
 * - LOG_* makrot karsiutuvat pois compile-timessa
 * - toteutuksen debug_logging.cpp-funktiot muuttuvat käytännössä no-opiksi
 *
 * Tällä tavalla sama koodipohja voidaan kääntää joko hyvin puheliaana
 * debug-versiona tai hyvin hiljaisena tuotantoversiona.
 */
#ifndef DEBUG_LOG_ENABLE
#define DEBUG_LOG_ENABLE 1
#endif

/*
 * Aktiivinen logitaso.
 *
 * Oletuksena DEBUG-taso, joka sopii hyvin:
 * - porttausvaiheeseen
 * - mittaustopologian testaukseen
 * - ADC- ja GPIO-sekvenssien analysointiin
 *
 * Myöhemmin tasoa voi säätää esimerkiksi näin:
 *
 *   #define DEBUG_LOG_ACTIVE_SEVERITY DEBUG_LOG_SEVERITY_INFO_VALUE
 *
 * jolloin DEBUG- ja TRACE-viestit katoavat, mutta INFO/WARN/ERROR säilyvät.
 */
#ifndef DEBUG_LOG_ACTIVE_SEVERITY
#define DEBUG_LOG_ACTIVE_SEVERITY DEBUG_LOG_SEVERITY_DEBUG_VALUE
#endif

/*
 * ============================================================================
 * Sisäisten puskureiden oletuskoot
 * ============================================================================
 *
 * Nämä arvot ovat toteutuksen käyttämiä oletuskokoja.
 *
 * DEBUG_LOG_FORMATTED_MESSAGE_MAX_LENGTH:
 *   Vain käyttäjän varsinainen viesti ennen prefixien lisäämistä.
 *
 * DEBUG_LOG_OUTPUT_LINE_MAX_LENGTH:
 *   Lopullinen ulostulorivi, jossa on mukana myös taso-, tagi- ja
 *   mahdolliset file/line-prefixit.
 *
 * Näitä voi tarvittaessa säätää projektikohtaisesti ennen tämän headerin
 * includea, jos jokin moduuli tarvitsee poikkeuksellisen pitkiä logirivejä.
 */
#ifndef DEBUG_LOG_FORMATTED_MESSAGE_MAX_LENGTH
#define DEBUG_LOG_FORMATTED_MESSAGE_MAX_LENGTH 160
#endif

#ifndef DEBUG_LOG_OUTPUT_LINE_MAX_LENGTH
#define DEBUG_LOG_OUTPUT_LINE_MAX_LENGTH 240
#endif

/*
 * ============================================================================
 * Julkinen funktiorajapinta
 * ============================================================================
 */

/*
 * Kirjoittaa yhden valmiiksi formatoidun logiviestin.
 *
 * Parametrit:
 * - severity:
 *     Logitason enum.
 *
 * - tag:
 *     Moduulitunniste, esimerkiksi:
 *       "CORE"
 *       "ADC"
 *       "TOPO"
 *       "TEST"
 *
 * - file:
 *     Lähdetiedoston nimi. Voi olla NULL, jos tiedostotietoa ei haluta mukaan.
 *
 * - line:
 *     Lähderivin numero. Yleensä 0, jos file == NULL.
 *
 * - format:
 *     printf-tyylinen formaattijono.
 *
 * Tämä funktio on LOG_* makrojen varsinainen backend.
 * Useimmissa tapauksissa sitä ei kutsuta suoraan sovelluskoodista,
 * vaan makrojen kautta.
 */
void debug_logging_write_message(
    debug_log_severity_t severity,
    const char *tag,
    const char *file,
    int line,
    const char *format,
    ...);

/*
 * Va-list-pohjainen versio.
 *
 * Tämä pidetään erillisenä, jotta:
 * - toteutus pysyy siistinä
 * - wrapper-funktiot on helppo rakentaa sen päälle
 * - vanha debug_logging_printf()-rajapinta voidaan toteuttaa
 *   ilman koodin duplikointia
 */
void debug_logging_write_message_v(
    debug_log_severity_t severity,
    const char *tag,
    const char *file,
    int line,
    const char *format,
    va_list args);

/*
 * Taaksepäin yhteensopiva printf-rajapinta.
 *
 * Nykyinen measurement_core.c käytti aiemmin tätä paljon, joten
 * rajapinta pidetään mukana siirtymävaiheessa.
 *
 * Tämä voidaan säilyttää toimivana, vaikka uusi suositeltu käyttöliittymä on:
 *   LOG_ERROR
 *   LOG_WARN
 *   LOG_INFO
 *   LOG_DEBUG
 *   LOG_TRACE
 *
 * Tavoite on, että vanhat kutsut voidaan korvata vähitellen uusilla
 * makroilla ilman että koko projektia tarvitsee kirjoittaa uusiksi kerralla.
 */
void debug_logging_printf(const char *format, ...);

/*
 * ============================================================================
 * Selkeä ja korkean tason makrorajapinta
 * ============================================================================
 *
 * Käyttöesimerkki:
 *
 *   LOG_INFO("CORE", "SAFE raw=%u V=%.4f", raw, (double)voltage);
 *   LOG_ERROR("ADC", "Channel read failed");
 *   LOG_DEBUG("TOPO", "Applied mode=%s", mode_name);
 *
 * Miksi makroja käytetään:
 *
 * 1. Käyttö on selkeämpää ja yhdenmukaisempaa koko projektissa
 * 2. Tiedosto/rivitieto voidaan lisätä helposti ERROR-tasolle
 * 3. Viestejä voidaan suodattaa compile-timessa ilman että kutsukohtia
 *    tarvitsee muuttaa
 *
 * Huom varargs-käytöstä:
 * - format on printf-tyylinen formaattijono
 * - ... tarkoittaa valinnaisia lisäargumentteja
 * - ##__VA_ARGS__ mahdollistaa myös tämän kaltaiset kutsut:
 *
 *     LOG_INFO("APP", "Boot complete");
 *
 *   jolloin ylimääräisiä formaattiargumentteja ei tarvitse antaa
 */
#if DEBUG_LOG_ENABLE

    /*
     * Error-tason logi.
     *
     * Error-tasolla mukaan lisätään automaattisesti myös __FILE__ ja __LINE__,
     * jotta virheen alkuperä löytyy nopeasti porttaus- ja debug-vaiheessa.
     */
    #define LOG_ERROR(tag, format, ...) \
        do { \
            if (DEBUG_LOG_ACTIVE_SEVERITY >= DEBUG_LOG_SEVERITY_ERROR_VALUE) { \
                debug_logging_write_message( \
                    DEBUG_LOG_SEVERITY_ERROR, \
                    tag, \
                    __FILE__, \
                    __LINE__, \
                    format, \
                    ##__VA_ARGS__); \
            } \
        } while (0)

    /*
     * Warning-tason logi.
     *
     * Warningeihin ei oletuksena lisätä file/line-tietoa, jotta loki pysyy
     * hieman siistimpänä ja kompaktimpana kuin virhetapauksissa.
     */
    #define LOG_WARN(tag, format, ...) \
        do { \
            if (DEBUG_LOG_ACTIVE_SEVERITY >= DEBUG_LOG_SEVERITY_WARN_VALUE) { \
                debug_logging_write_message( \
                    DEBUG_LOG_SEVERITY_WARN, \
                    tag, \
                    NULL, \
                    0, \
                    format, \
                    ##__VA_ARGS__); \
            } \
        } while (0)

    /*
     * Info-tason logi.
     *
     * Tätä kannattaa käyttää esimerkiksi:
     * - tilasiirtymiin
     * - korkean tason sekvenssivaiheisiin
     * - mittausajon etenemiseen
     */
    #define LOG_INFO(tag, format, ...) \
        do { \
            if (DEBUG_LOG_ACTIVE_SEVERITY >= DEBUG_LOG_SEVERITY_INFO_VALUE) { \
                debug_logging_write_message( \
                    DEBUG_LOG_SEVERITY_INFO, \
                    tag, \
                    NULL, \
                    0, \
                    format, \
                    ##__VA_ARGS__); \
            } \
        } while (0)

    /*
     * Debug-tason logi.
     *
     * Tätä kannattaa käyttää esimerkiksi:
     * - ADC raw / voltage -lukuihin
     * - topologian yksityiskohtaiseen tilaan
     * - välituloksiin ja tarkempaan diagnostiikkaan
     */
    #define LOG_DEBUG(tag, format, ...) \
        do { \
            if (DEBUG_LOG_ACTIVE_SEVERITY >= DEBUG_LOG_SEVERITY_DEBUG_VALUE) { \
                debug_logging_write_message( \
                    DEBUG_LOG_SEVERITY_DEBUG, \
                    tag, \
                    NULL, \
                    0, \
                    format, \
                    ##__VA_ARGS__); \
            } \
        } while (0)

    /*
     * Trace-tason logi.
     *
     * Trace on kaikkein yksityiskohtaisin taso. Sitä ei välttämättä tarvita
     * normaalissa käytössä, mutta se on hyödyllinen jos halutaan tarkastella
     * erittäin hienojakoista toimintaa tai aikajärjestystä.
     */
    #define LOG_TRACE(tag, format, ...) \
        do { \
            if (DEBUG_LOG_ACTIVE_SEVERITY >= DEBUG_LOG_SEVERITY_TRACE_VALUE) { \
                debug_logging_write_message( \
                    DEBUG_LOG_SEVERITY_TRACE, \
                    tag, \
                    NULL, \
                    0, \
                    format, \
                    ##__VA_ARGS__); \
            } \
        } while (0)

#else

    /*
     * Kun loggaus on kytketty pois, makrot katoavat käytännössä kokonaan.
     *
     * do { } while (0) -rakenne pitää makrot syntaktisesti turvallisina myös
     * if/else-käytössä.
     */
    #define LOG_ERROR(tag, format, ...) do { } while (0)
    #define LOG_WARN(tag, format, ...)  do { } while (0)
    #define LOG_INFO(tag, format, ...)  do { } while (0)
    #define LOG_DEBUG(tag, format, ...) do { } while (0)
    #define LOG_TRACE(tag, format, ...) do { } while (0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_LOGGING_H */
