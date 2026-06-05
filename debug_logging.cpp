/*
 * STM32duino-taustainen debug-loggaustoteutus.
 *
 * Tämä tiedosto on tarkoituksella C++-tiedosto (.cpp), koska Arduino-
 * ympäristön Serial-rajapinta on C++-objekti eikä puhdasta C:tä.
 *
 * Tämän tiedoston tehtävä on toimia yhteisenä loggausbackendinä koko
 * mittausprojektille:
 *
 * - Mittausmoduulit voivat käyttää samoja LOG_ERROR / LOG_WARN /
 *   LOG_INFO / LOG_DEBUG / LOG_TRACE -makroja riippumatta siitä,
 *   ollaanko STM32CubeIDE/HAL- vai STM32duino-portissa.
 *
 * - Tällä hetkellä ulostulo kirjoitetaan Arduino Serial -rajapinnan
 *   kautta sarjamonitoriin.
 *
 * - Myöhemmin backend voidaan vaihtaa keskitetysti esimerkiksi
 *   HAL_UART_Transmit()-pohjaiseksi, USB CDC -toteutukseksi tai
 *   vaikka rengaspuskuriin, ilman että mittauslogiikkaa tarvitsee
 *   kirjoittaa uudelleen.
 *
 * NUCLEO-L432KC:n STM32duino-variantissa oletus-Serial on sidottu
 * ST-LINKin Virtual COM Portiin, joten tämä tiedosto toimii
 * käytännössä suoraan oikealla board-valinnalla ja Serial.begin(...)
 * alustuksen jälkeen.
 */

#include "debug_logging.h"

#include <Arduino.h>
#include <stdio.h>

/*
 * Muuntaa logitason lyhyeksi tekstiesitykseksi.
 *
 * Tarkoitus on pitää lokirivit kompakteina mutta helposti luettavina.
 * Yhden kirjaimen tunnukset ovat riittäviä sarjamonitorissa:
 *
 *   [E] = Error
 *   [W] = Warn
 *   [I] = Info
 *   [D] = Debug
 *   [T] = Trace
 *
 * Tämä funktio on sisäinen helperi eikä näy moduulin ulkoisessa
 * rajapinnassa.
 */
static const char *debug_logging_get_severity_text(debug_log_severity_t severity)
{
    switch (severity)
    {
        case DEBUG_LOG_SEVERITY_ERROR:
            return "E";

        case DEBUG_LOG_SEVERITY_WARN:
            return "W";

        case DEBUG_LOG_SEVERITY_INFO:
            return "I";

        case DEBUG_LOG_SEVERITY_DEBUG:
            return "D";

        case DEBUG_LOG_SEVERITY_TRACE:
            return "T";

        default:
            return "?";
    }
}

/*
 * Kirjoittaa yhden valmiiksi rakennetun logirivin fyysiseen ulostuloon.
 *
 * Tämä on tarkoituksella erotettu omaksi helperikseen, jotta:
 * - varsinainen viestin formatointi pysyy erillään
 * - backend voidaan vaihtaa myöhemmin yhdestä paikasta
 * - muu toteutus pysyy siistinä ja helposti ylläpidettävänä
 *
 * Tällä hetkellä ulostulo menee suoraan Serial.println()-kutsuun.
 * Jos loggaus on compile-timessa kytketty pois, funktio muuttuu
 * käytännössä no-opiksi.
 */
static void debug_logging_emit_line(const char *line)
{
#if DEBUG_LOG_ENABLE
    if (line == nullptr)
    {
        return;
    }

    /*
     * Serial.println() lisää rivinvaihdon automaattisesti.
     * Tämä on sopiva oletus sarjamonitorikäyttöön.
     */
    Serial.println(line);
#else
    (void)line;
#endif
}

void debug_logging_write_message_v(
    debug_log_severity_t severity,
    const char *tag,
    const char *file,
    int line,
    const char *format,
    va_list args)
{
#if DEBUG_LOG_ENABLE
    if (format == nullptr)
    {
        return;
    }

    /*
     * Muotoillaan ensin käyttäjän varsinainen logiviesti.
     *
     * Tämä puskuri sisältää vain itse sanoman, esimerkiksi:
     *   "SAFE raw=1234 V=0.9940"
     * tai:
     *   "Topology apply failed"
     *
     * Varsinaiset taso-, tagi- ja mahdolliset tiedosto/riviprefixit
     * lisätään vasta seuraavassa vaiheessa.
     */
    char formatted_message[DEBUG_LOG_FORMATTED_MESSAGE_MAX_LENGTH];
    vsnprintf(formatted_message, sizeof(formatted_message), format, args);

    /*
     * Rakennetaan lopullinen ulostulorivi.
     *
     * Perusmuoto:
     *   [I][CORE] message
     *
     * Error-viesteille voidaan liittää myös tiedosto ja rivinumero:
     *   [E][ADC][file.c:123] message
     *
     * Tämä on erityisen hyödyllinen porttaus- ja debug-vaiheessa,
     * kun halutaan nopeasti tunnistaa, mistä virhe on tullut.
     */
    char output_line[DEBUG_LOG_OUTPUT_LINE_MAX_LENGTH];

    /*
     * Jos tagia ei ole annettu, käytetään oletuksena "APP".
     * Tämä pitää tulostusformaatin aina yhtenäisenä.
     */
    const char *safe_tag = (tag != nullptr) ? tag : "APP";
    const char *severity_text = debug_logging_get_severity_text(severity);

    /*
     * Error-tason viesteihin lisätään tiedosto/rivi, jos ne on annettu.
     * Muilla tasoilla pidetään loki hieman siistimpänä ja lyhyempänä.
     */
    if ((severity == DEBUG_LOG_SEVERITY_ERROR) &&
        (file != nullptr) &&
        (line > 0))
    {
        snprintf(
            output_line,
            sizeof(output_line),
            "[%s][%s][%s:%d] %s",
            severity_text,
            safe_tag,
            file,
            line,
            formatted_message);
    }
    else
    {
        snprintf(
            output_line,
            sizeof(output_line),
            "[%s][%s] %s",
            severity_text,
            safe_tag,
            formatted_message);
    }

    /*
     * Lopullinen rivi lähetetään varsinaiselle ulostulobackendille.
     */
    debug_logging_emit_line(output_line);
#else
    /*
     * Kun loggaus on compile-timessa pois päältä, pidetään funktio
     * edelleen olemassa mutta tehdään siitä no-op. Tämä pitää linkityksen
     * ja rajapinnan vakaana myös silloin, kun vanhaa kutsukoodia on
     * vielä jäljellä projektissa.
     */
    (void)severity;
    (void)tag;
    (void)file;
    (void)line;
    (void)format;
    (void)args;
#endif
}

void debug_logging_write_message(
    debug_log_severity_t severity,
    const char *tag,
    const char *file,
    int line,
    const char *format,
    ...)
{
#if DEBUG_LOG_ENABLE
    /*
     * Tämä on normaalin käytön pääsisäänmeno toteutukseen.
     * Makrot LOG_ERROR / LOG_INFO / LOG_DEBUG jne. päätyvät lopulta
     * kutsumaan tätä funktiota.
     */
    va_list args;
    va_start(args, format);
    debug_logging_write_message_v(severity, tag, file, line, format, args);
    va_end(args);
#else
    (void)severity;
    (void)tag;
    (void)file;
    (void)line;
    (void)format;
#endif
}

void debug_logging_printf(const char *format, ...)
{
#if DEBUG_LOG_ENABLE
    /*
     * Taaksepäin yhteensopiva fallback-rajapinta.
     *
     * Tämä on mukana siksi, että projektissa on jo olemassa vanhoja
     * debug_logging_printf()-kutsuja, erityisesti measurement_core.c:ssä.
     *
     * Näin saadaan uusi loggausinfrastruktuuri käyttöön ilman että koko
     * koodipohjaa tarvitsee refaktoroida yhdellä kertaa.
     *
     * Oletus:
     * - severity = INFO
     * - tag      = "APP"
     *
     * Myöhemmin nämä vanhat kutsut kannattaa asteittain korvata
     * selkeämmillä LOG_INFO / LOG_DEBUG / LOG_ERROR -makroilla.
     */
    if (format == nullptr)
    {
        return;
    }

    va_list args;
    va_start(args, format);
    debug_logging_write_message_v(
        DEBUG_LOG_SEVERITY_INFO,
        "APP",
        nullptr,
        0,
        format,
        args);
    va_end(args);
#else
    (void)format;
#endif
}
