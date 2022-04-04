#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re
import argparse
import configparser
import datetime
import html
import math
import smtplib
from os.path import basename
from email.mime.application import MIMEApplication
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from email.utils import formatdate
from email import charset

from mutagen.mp3 import MP3


def send_mail(args):
    config = configparser.ConfigParser()
    config.read(args.config)
    config = config['mail']

    subject = config['subject']
    send_from = config['send_from']
    send_to = config['send_to']

    dt = datetime.datetime.now()
    try:
        audio = MP3(args.filename)
        delta = datetime.timedelta(math.ceil(audio.info.length))
        length = str(delta)
        dt -= delta
    except Exception as exc:
        length = str(exc)

    contents = {
        'caller': args.caller,
        'name': args.name,
        'number': args.number,
        'date': str(dt.date()),
        'time': dt.time().strftime('%H:%S'),
        'length': length,
        'subject': subject,
        'title': config['title'],
        'title_html': config['title_html'],
        'epilog_html': config['epilog_html'],
        'label_from': config['label_from'],
        'label_for': config['label_for'],
        'label_date': config['label_date'],
        'label_time': config['label_time'],
        'label_length': config['label_length'],
    }
    contents['content'] = config['content'].format(**contents)

    msg = MIMEMultipart(boundary="==Voice_Box==multipart/mixed==0==")
    msg['From'] = send_from
    msg['To'] = send_to
    msg['Date'] = formatdate(localtime=True)
    msg['Subject'] = subject.format(**contents)

    cs = charset.Charset('utf-8')
    cs.body_encoding = charset.QP

    body = config.get('body', '''{title}:

{content}

{label_from}: {name} ({caller})
{label_for}: {number}
{label_date}: {date}
{label_time}: {time}
{label_length}: {length}
''')
    text = MIMEMultipart('alternative', boundary="==Voice_Box==multipart/alternative==1==")
    text.attach(MIMEText(body.format(**contents), 'plain', cs))
    try:
        with open('mail.html') as fd:
            body_html = re.sub(r'^\s*', '', fd.read())
        text.attach(MIMEText(body_html.format(**{key: html.escape(value) for key, value in contents.items()}), 'html', cs))
    except IOError:
        pass
    msg.attach(text)

    with open(args.filename, "rb") as fd:
        part = MIMEApplication(
            fd.read(),
            Name=basename(args.filename)
        )
        filename = basename(args.filename).replace('"', '').replace('\\', '')
        part['Content-Disposition'] = 'attachment; filename="%s"' % (filename,)
        msg.attach(part)

    with smtplib.SMTP(config['server'], int(config['port'])) as s:
        s.ehlo()
        s.starttls()
        s.login(send_from, config['password'])
        s.sendmail(send_from, send_to, msg.as_string())
    print('OK: email sent')

# FIXME: handle such errors. What to do then?
# Traceback (most recent call last):
#   File "./mail.py", line 102, in <module>
#     send_mail(args)
#   File "./mail.py", line 91, in send_mail
#     s.sendmail(send_from, send_to, msg.as_string())
#   File "/usr/lib/python3.7/smtplib.py", line 888, in sendmail
#     raise SMTPDataError(code, resp)
# smtplib.SMTPDataError: (554, b'5.7.0 Your message could not be sent. The limit on the number of allowed outgoing messages was exceeded. Try again later.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', default='mail.cfg')
    parser.add_argument('--name', default='Name unknown')
    parser.add_argument('number')
    parser.add_argument('caller')
    parser.add_argument('filename')
    args = parser.parse_args()
    send_mail(args)
