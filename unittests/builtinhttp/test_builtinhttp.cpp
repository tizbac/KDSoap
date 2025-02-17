/****************************************************************************
**
** This file is part of the KD Soap project.
**
** SPDX-FileCopyrightText: 2010-2023 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>
**
** SPDX-License-Identifier: MIT
**
****************************************************************************/

#include "KDSoapAuthentication.h"
#include "KDSoapClientInterface.h"
#include "KDSoapMessage.h"
#include "KDSoapNamespaceManager.h"
#include "KDSoapPendingCallWatcher.h"
#include "KDSoapServer.h"
#include "KDSoapServerObjectInterface.h"
#include "KDSoapValue.h"
#include "httpserver_p.h"

#include <QDebug>
#include <QEventLoop>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QTest>

using namespace KDSoapUnitTestHelpers;

class BuiltinHttpTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // Test that we can use asyncCall without using a watcher, just waiting and checking isFinished.
    void testAsyncCall()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::Public);

        qDebug() << "server ready, proceeding" << server.endPoint();
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        KDSoapPendingCall call = client.asyncCall(QLatin1String("getEmployeeCountry"), countryMessage());
        QVERIFY(!call.isFinished());
        QTest::qWait(1000);
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedCountryRequest()));
        QVERIFY(call.isFinished());
        QCOMPARE(call.returnMessage().arguments().child(QLatin1String("employeeCountry")).value().toString(), QString::fromLatin1("France"));
    }

    void testFaultSoap11() // HTTP error, creates fault on client side
    {
        HttpServerThread server(QByteArray(), HttpServerThread::Public | HttpServerThread::Error404);
        KDSoapClientInterface client(server.endPoint(), QString::fromLatin1("urn:msg"));
        client.setSoapVersion(KDSoapClientInterface::SOAP1_1);
        KDSoapMessage message;
        KDSoapMessage ret = client.call(QLatin1String("Method1"), message);
        QVERIFY(ret.isFault());
        QCOMPARE(ret.faultAsString(),
                 QString::fromLatin1("Fault code 203: Error transferring %1 - server replied: Not Found").arg(server.endPoint()));
        QCOMPARE(
            QString::fromLatin1(ret.toXml().constData()),
            QLatin1String(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<soap:Fault xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\" xmlns:soap-enc=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
                "<faultcode>203</faultcode>"
                "<faultstring>Error transferring ")
                + server.endPoint()
                + QLatin1String(" - server replied: Not Found</faultstring>"
                                "</soap:Fault>\n"));
    }

    void testFaultSoap12() // HTTP error, creates fault on client side
    {
        HttpServerThread server(QByteArray(), HttpServerThread::Public | HttpServerThread::Error404);
        KDSoapClientInterface client(server.endPoint(), QString::fromLatin1("urn:msg"));
        client.setSoapVersion(KDSoapClientInterface::SOAP1_2);
        KDSoapMessage message;
        KDSoapMessage ret = client.call(QLatin1String("Method1"), message);
        QVERIFY(ret.isFault());
        QCOMPARE(ret.faultAsString(), QString::fromLatin1("Fault 203: Error transferring %1 - server replied: Not Found").arg(server.endPoint()));
    }

    void testInvalidXML()
    {
        HttpServerThread server(QByteArray(xmlEnvBegin11()) + "><soap:Body><broken></xml></soap:Body>", HttpServerThread::Public);
        KDSoapClientInterface client(server.endPoint(), QString::fromLatin1("urn:msg"));
        KDSoapMessage message;
        KDSoapMessage ret = client.call(QLatin1String("Method1"), message);
        QVERIFY(ret.isFault());
        QCOMPARE(ret.faultAsString(), QString::fromLatin1("Fault code 3: XML error: [1:291] Opening and ending tag mismatch."));
    }

    void testInvalidUndefinedEntityXML()
    {
        HttpServerThread server(QByteArray(xmlEnvBegin11()) + "><soap:Body>&doesnotexist;</soap:Body>" + xmlEnvEnd() + '\n',
                                HttpServerThread::Public);
        KDSoapClientInterface client(server.endPoint(), QString::fromLatin1("urn:msg"));
        KDSoapMessage message;
        KDSoapMessage ret = client.call(QLatin1String("Method1"), message);
        QVERIFY(ret.isFault());
        QCOMPARE(ret.faultAsString(), QString::fromLatin1("Fault code 3: XML error: [1:291] Entity 'doesnotexist' not declared."));
    }

    // Test for basic auth, with async call
    void testAsyncCallWithAuth()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::BasicAuth);
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        KDSoapAuthentication auth;
        auth.setUser(QLatin1String("kdab"));
        auth.setPassword(QLatin1String("testpass"));
        client.setAuthentication(auth);
        KDSoapPendingCall call = client.asyncCall(QLatin1String("getEmployeeCountry"), countryMessage());
        QVERIFY(!call.isFinished());
        waitForCallFinished(call);
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedCountryRequest()));
        QVERIFY(call.isFinished());
        QCOMPARE(call.returnMessage().arguments().child(QLatin1String("employeeCountry")).value().toString(), QString::fromLatin1("France"));
    }

    // Test for refused auth, with async call
    void testAsyncCallRefusedAuth()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::BasicAuth);
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        KDSoapAuthentication auth;
        auth.setUser(QLatin1String("kdab"));
        auth.setPassword(QLatin1String("invalid"));
        client.setAuthentication(auth);
        KDSoapPendingCall call = client.asyncCall(QLatin1String("getEmployeeCountry"), countryMessage());
        QVERIFY(!call.isFinished());
        waitForCallFinished(call);
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedCountryRequest()));
        QVERIFY(call.isFinished());
        QVERIFY(call.returnMessage().isFault());
    }

    // Test for refused auth, with sync call (i.e. in thread)
    void testCallRefusedAuth()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::BasicAuth);
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        KDSoapAuthentication auth;
        auth.setUser(QLatin1String("kdab"));
        auth.setPassword(QLatin1String("invalid"));
        client.setAuthentication(auth);
        KDSoapMessage reply = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
        QVERIFY(reply.isFault());
        QCOMPARE(reply.childValues().child(QLatin1String("faultcode")).value().toInt(), static_cast<int>(QNetworkReply::AuthenticationRequiredError));
    }

    void testCorrectHttpHeader()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::Public);
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        KDSoapAuthentication auth;

        auth.setUser(QLatin1String("kdab"));
        auth.setPassword(QLatin1String("unused"));
        client.setAuthentication(auth); // unused...

        QNetworkCookieJar myJar;
        QList<QNetworkCookie> myCookies;
        myCookies.append(QNetworkCookie("biscuits", "are good"));
        myJar.setCookiesFromUrl(myCookies, QUrl(server.endPoint()));
        client.setCookieJar(&myJar);

        QByteArray expectedRequestXml = expectedCountryRequest();
        client.setSoapVersion(KDSoapClientInterface::SOAP1_1);
        {
            KDSoapMessage ret =
                client.call(QLatin1String("getEmployeeCountry"), countryMessage(), "http://www.kdab.com/xml/MyWsdl/getEmployeeCountry");
            // Check what we sent
            QVERIFY(xmlBufferCompare(server.receivedData(), expectedRequestXml));
            QVERIFY(!ret.isFault());

            QCOMPARE(server.header("Content-Type").constData(), "text/xml;charset=utf-8");
            QCOMPARE(server.header("SoapAction").constData(), "\"http://www.kdab.com/xml/MyWsdl/getEmployeeCountry\"");
            QCOMPARE(server.header("Cookie").constData(), "biscuits=are good");
            QCOMPARE(ret.arguments().child(QLatin1String("employeeCountry")).value().toString(), QString::fromLatin1("France"));
        }
        client.setSoapVersion(KDSoapClientInterface::SOAP1_2);
        {
            KDSoapMessage ret =
                client.call(QLatin1String("getEmployeeCountry"), countryMessage(), "http://www.kdab.com/xml/MyWsdl/getEmployeeCountry");
            // Check what we sent
            QByteArray expectedRequestXml12 = expectedRequestXml;
            expectedRequestXml12.replace("http://schemas.xmlsoap.org/soap/envelope/", "http://www.w3.org/2003/05/soap-envelope");
            expectedRequestXml12.replace("http://schemas.xmlsoap.org/soap/encoding/", "http://www.w3.org/2003/05/soap-encoding");
            QVERIFY(xmlBufferCompare(server.receivedData(), expectedRequestXml12));
            QVERIFY(!ret.isFault());
            QCOMPARE(server.header("Content-Type").constData(),
                     "application/soap+xml;charset=utf-8;action=http://www.kdab.com/xml/MyWsdl/getEmployeeCountry");
            QCOMPARE(ret.arguments().child(QLatin1String("employeeCountry")).value().toString(), QString::fromLatin1("France"));
            QCOMPARE(server.header("Cookie").constData(), "biscuits=are good");
        }
    }
    // Using direct call(), check the xml we send, the response parsing.
    // Then test callNoReply, then various ways to use asyncCall.
    void testCallNoReply()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::Public);

        // First, make the proper call
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        KDSoapAuthentication auth;
        auth.setUser(QLatin1String("kdab"));
        auth.setPassword(QLatin1String("unused"));
        client.setAuthentication(auth); // unused...
        QByteArray expectedRequestXml = expectedCountryRequest();

        {
            KDSoapMessage ret = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
            // Check what we sent
            QVERIFY(xmlBufferCompare(server.receivedData(), expectedRequestXml));
            QVERIFY(!ret.isFault());
            QCOMPARE(ret.arguments().child(QLatin1String("employeeCountry")).value().toString(), QString::fromLatin1("France"));
        }

        // Now make the call again, but async, and don't wait for response.
        server.resetReceivedBuffers();
        // qDebug() << "== now calling callNoReply ==";
        client.callNoReply(QLatin1String("getEmployeeCountry"), countryMessage());
        QTest::qWait(200);
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedRequestXml));

        // What happens if we use asyncCall and discard the result?
        // The KDSoapPendingCall goes out of scope, and the network request is aborted.
        //
        // The whole reason KDSoapPendingCall is a value, is so that people don't forget
        // to delete it. But of course if they even forget to store it, nothing happens.
        server.resetReceivedBuffers();
        {
            client.asyncCall(QLatin1String("getEmployeeCountry"), countryMessage());
            QTest::qWait(200);
        }
        QVERIFY(server.receivedData().isEmpty());
    }

    void testRequestXml() // this tests the serialization of KDSoapValue[List] in KDSoapClientInterface
    {
        HttpServerThread server(emptyResponse(), HttpServerThread::Public);
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        KDSoapMessage message;
        message.setUse(KDSoapMessage::EncodedUse); // write out types explicitly

        // Test simpletype element
        message.addArgument(QString::fromLatin1("testString"), QString::fromUtf8("Hello Klarälvdalens"));

        // Test simpletype extended to have an attribute
        KDSoapValue val(QString::fromLatin1("val"), 5, countryMessageNamespace(), QString::fromLatin1("MyVal"));
        val.childValues().attributes().append(KDSoapValue(QString::fromLatin1("attr"), QString::fromLatin1("attrValue")));
        message.arguments().append(val);

        // Test complextype with child elements and attributes
        KDSoapValueList valueList;
        KDSoapValue orderperson(QString::fromLatin1("orderperson"), QString::fromLatin1("someone"), countryMessageNamespace(),
                                QString::fromLatin1("Person"));
        valueList.append(orderperson);
        valueList.attributes().append(KDSoapValue(QString::fromLatin1("attr"), QString::fromLatin1("attrValue")));
        message.addArgument(QString::fromLatin1("order"), valueList, countryMessageNamespace(), QString::fromLatin1("MyOrder"));

        // Code from documentation
        QRect rect(0, 0, 100, 200);
        KDSoapValueList rectArgument;
        rectArgument.addArgument(QLatin1String("x"), rect.x());
        rectArgument.addArgument(QLatin1String("y"), rect.y());
        rectArgument.addArgument(QLatin1String("width"), rect.width());
        rectArgument.addArgument(QLatin1String("height"), rect.height());
        message.addArgument(QLatin1String("rect"), rectArgument); // NOTE: type information is missing; setQualified() is missing too.

        const QString XMLSchemaNS = KDSoapNamespaceManager::xmlSchema2001();

        // Test array
        KDSoapValueList arrayContents;
        arrayContents.setArrayType(XMLSchemaNS, QString::fromLatin1("string"));
        arrayContents.append(KDSoapValue(QString::fromLatin1("item"), QString::fromLatin1("kdab"), XMLSchemaNS, QString::fromLatin1("string")));
        arrayContents.append(KDSoapValue(QString::fromLatin1("item"), QString::fromLatin1("rocks"), XMLSchemaNS, QString::fromLatin1("string")));
        message.addArgument(QString::fromLatin1("testArray"), arrayContents, QString::fromLatin1("http://schemas.xmlsoap.org/soap/encoding/"),
                            QString::fromLatin1("Array"));

        // Add a header
        KDSoapMessage header1;
        header1.setUse(KDSoapMessage::EncodedUse);
        header1.addArgument(QString::fromLatin1("header1"), QString::fromLatin1("headerValue"));
        KDSoapHeaders headers;
        headers << header1;

        client.call(QLatin1String("test"), message, QString::fromLatin1("MySoapAction"), headers);
        // Check what we sent
        QByteArray expectedRequestXml = QByteArray(xmlEnvBegin11())
            + " xmlns:n1=\"http://www.kdab.com/xml/MyWsdl/\""
              "><soap:Header>"
              "<n1:header1 xsi:type=\"xsd:string\">headerValue</n1:header1>"
              "</soap:Header>";
        const QByteArray expectedRequestBody =
            QByteArray("<soap:Body>"
                       "<n1:test>"
                       "<testString xsi:type=\"xsd:string\">Hello Klarälvdalens</testString>"
                       "<val xsi:type=\"n1:MyVal\" attr=\"attrValue\">5</val>"
                       "<order xsi:type=\"n1:MyOrder\" attr=\"attrValue\"><orderperson xsi:type=\"n1:Person\">someone</orderperson></order>"
                       "<rect><x xsi:type=\"xsd:int\">0</x><y xsi:type=\"xsd:int\">0</y><width xsi:type=\"xsd:int\">100</width><height "
                       "xsi:type=\"xsd:int\">200</height></rect>"
                       "<testArray xsi:type=\"soap-enc:Array\" soap-enc:arrayType=\"xsd:string[2]\">"
                       "<item xsi:type=\"xsd:string\">kdab</item>"
                       "<item xsi:type=\"xsd:string\">rocks</item>"
                       "</testArray>"
                       "</n1:test>"
                       "</soap:Body>")
            + xmlEnvEnd() + '\n'; // added by QXmlStreamWriter::writeEndDocument
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedRequestXml + expectedRequestBody));

        // Now using persistent headers
        server.resetReceivedBuffers();
        client.setHeader(QLatin1String("header1"), header1);
        client.call(QLatin1String("test"), message, QString::fromLatin1("MySoapAction"));
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedRequestXml + expectedRequestBody));

        // Now remove the persistent header (using setHeader + empty message)
        server.resetReceivedBuffers();
        client.setHeader(QLatin1String("header1"), KDSoapMessage());
        client.call(QLatin1String("test"), message, QString::fromLatin1("MySoapAction"));
        const QByteArray expectedRequestXmlNoHeader = QByteArray(xmlEnvBegin11())
            + " xmlns:n1=\"http://www.kdab.com/xml/MyWsdl/\""
              "><soap:Header/>"; // the empty element does not matter
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedRequestXmlNoHeader + expectedRequestBody));
    }

    // Test parsing of complex replies, like with SugarCRM
    void testParseComplexReply()
    {
        HttpServerThread server(complexTypeResponse(), HttpServerThread::Public);
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
        QVERIFY(!response.isFault());
        QCOMPARE(response.arguments().count(), 1);
        const KDSoapValueList lst = response.arguments().first().childValues();
        QCOMPARE(lst.count(), 3);
        const KDSoapValue id = lst.first();
        QCOMPARE(id.name(), QString::fromLatin1("id"));
        QCOMPARE(id.value().toString(), QString::fromLatin1("12345"));
        const KDSoapValue error = lst.at(1);
        QCOMPARE(error.name(), QString::fromLatin1("error"));
        const KDSoapValueList errorList = error.childValues();
        QCOMPARE(errorList.count(), 3);
        const KDSoapValue number = errorList.at(0);
        QCOMPARE(number.name(), QString::fromLatin1("number"));
        QCOMPARE(number.value().toString(), QString::fromLatin1("0"));
        const KDSoapValue name = errorList.at(1);
        QCOMPARE(name.name(), QString::fromLatin1("name"));
        QCOMPARE(name.value().toString(), QString::fromLatin1("No Error"));
        const KDSoapValue description = errorList.at(2);
        QCOMPARE(description.name(), QString::fromLatin1("description"));
        QCOMPARE(description.value().toString(), QString::fromLatin1("No Error"));
        const KDSoapValue array = lst.at(2);
        QCOMPARE(array.name(), QString::fromLatin1("testArray"));
        // qDebug() << array;

        // Code from documentation
        const QString sessionId = response.arguments()[0].value().toString();
        Q_UNUSED(sessionId);
    }

    void testDocumentStyle()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::Public);

        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        client.setStyle(KDSoapClientInterface::DocumentStyle);
        QByteArray expectedRequestXml = expectedCountryRequest();

        KDSoapMessage message;
        message = KDSoapValue(QLatin1String("getEmployeeCountry"), QVariant());
        KDSoapValueList &args = message.childValues();
        args.append(KDSoapValue(QLatin1String("employeeName"), QString::fromUtf8("David Ä Faure")));

        {
            KDSoapMessage ret = client.call(QLatin1String("UNUSED"), message);
            // Check what we sent
            QVERIFY(xmlBufferCompare(server.receivedData(), expectedRequestXml));
            QVERIFY(!ret.isFault());
            QCOMPARE(ret.arguments().child(QLatin1String("employeeCountry")).value().toString(), QString::fromLatin1("France"));
        }
    }

    // Test if the setClientSendsActionInHttpHeader is set false in the client then the SOAP action
    // is really not placed into the Content-Type HTTP header
    void testSetClientSendsActionInHttpHeader()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::Public);
        server.setClientSendsActionInHttpHeader(false);

        qDebug() << "server ready, proceeding" << server.endPoint();
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        client.setSendSoapActionInHttpHeader(false);
        client.setSoapVersion(KDSoapClientInterface::SOAP1_2);
        QString action = QStringLiteral("http://localhost/getEmployeeCountry");
        server.setExpectedSoapAction(action.toLocal8Bit());
        KDSoapMessage ret = client.call(QStringLiteral("getEmployeeCountry"), countryMessage(), action);
        QVERIFY(!ret.isFault());
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedCountryRequest12()));
        QCOMPARE(server.header("Content-Type").constData(), "application/soap+xml;charset=utf-8");
    }

    // Test if the setSendSoapActionInWsAddressingHeader is set true in the client then the SOAP action
    // is really placed in the SOAP envelope's header section
    void testSendSoapActionWithWSAddressing()
    {
        HttpServerThread server(countryResponse(), HttpServerThread::Public);

        qDebug() << "server ready, proceeding" << server.endPoint();
        KDSoapClientInterface client(server.endPoint(), countryMessageNamespace());
        client.setSendSoapActionInWsAddressingHeader(true);
        client.setSoapVersion(KDSoapClientInterface::SOAP1_2);
        QString action = QStringLiteral("http://localhost/getEmployeeCountry");
        server.setExpectedSoapAction(action.toLocal8Bit());
        KDSoapMessage ret = client.call(QStringLiteral("getEmployeeCountry"), countryMessage(), action);
        QVERIFY(!ret.isFault());
        QVERIFY(xmlBufferCompare(server.receivedData(), expectedCountryRequestWithWSAddressingAction()));
    }


private:
    static QByteArray countryResponse()
    {
        return QByteArray(xmlEnvBegin11())
            + "><soap:Body>"
              "<kdab:getEmployeeCountryResponse "
              "xmlns:kdab=\"http://www.kdab.com/xml/MyWsdl/\"><kdab:employeeCountry>France</kdab:employeeCountry></kdab:getEmployeeCountryResponse>"
              " </soap:Body>"
            + xmlEnvEnd();
    }
    static QByteArray expectedCountryRequest()
    {
        return QByteArray(xmlEnvBegin11())
            + "><soap:Body>"
              "<n1:getEmployeeCountry xmlns:n1=\"http://www.kdab.com/xml/MyWsdl/\">"
              "<employeeName>David Ä Faure</employeeName>"
              "</n1:getEmployeeCountry>"
              "</soap:Body>"
            + xmlEnvEnd();
    }
    static QByteArray expectedCountryRequest12()
    {
        return QByteArray(xmlEnvBegin12()) + "><soap:Body>"
                                             "<n1:getEmployeeCountry xmlns:n1=\"http://www.kdab.com/xml/MyWsdl/\">"
                                             "<employeeName>David Ä Faure</employeeName>"
                                             "</n1:getEmployeeCountry>"
                                             "</soap:Body>"
            + xmlEnvEnd();
    }
    static QByteArray expectedCountryRequestWithWSAddressingAction()
    {
        return QByteArray(xmlEnvBegin12WithWSAddressing()) + "><soap:Header><wsa:Action>http://localhost/getEmployeeCountry</wsa:Action></soap:Header>"
                                                             "<soap:Body>"
                                                             "<n1:getEmployeeCountry>"
                                                             "<employeeName>David Ä Faure</employeeName>"
                                                             "</n1:getEmployeeCountry>"
                                                             "</soap:Body>"
            + xmlEnvEnd();
    }
    static QString countryMessageNamespace()
    {
        return QString::fromLatin1("http://www.kdab.com/xml/MyWsdl/");
    }
    static KDSoapMessage countryMessage()
    {
        KDSoapMessage message;
        message.addArgument(QLatin1String("employeeName"), QString::fromUtf8("David Ä Faure"));
        return message;
    }
    void waitForCallFinished(KDSoapPendingCall &pendingCall)
    {
        KDSoapPendingCallWatcher *watcher = new KDSoapPendingCallWatcher(pendingCall, this);
        QEventLoop loop;
        connect(watcher, &KDSoapPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }

    static QByteArray emptyResponse()
    {
        return QByteArray(xmlEnvBegin11()) + "><soap:Body/>";
    }

    static QByteArray complexTypeResponse()
    {
        return QByteArray(xmlEnvBegin11())
            + "><soap:Body xmlns:tns=\"http://www.sugarcrm.com/sugarcrm\">"
              "<ns1:loginResponse xmlns:ns1=\"http://www.sugarcrm.com/sugarcrm\">"
              "  <return xsi:type=\"tns:set_entry_result\">"
              "    <id xsi:type=\"xsd:string\">12345</id>"
              "    <error xsi:type=\"tns:error_value\">"
              "       <number xsi:type=\"xsd:string\">0</number>"
              "       <name xsi:type=\"xsd:string\">No Error</name>"
              "       <description xsi:type=\"xsd:string\">No Error</description>"
              "    </error>"
              "    <testArray ns1:attr=\"aValue\" xsi:type=\"soap-enc:Array\" soap-enc:arrayType=\"xsi:string\">"
              "    </testArray>"
              "  </return>"
              "</ns1:loginResponse>"
              "</soap:Body>"
            + xmlEnvEnd();
    }
};

QTEST_MAIN(BuiltinHttpTest)

#include "test_builtinhttp.moc"
